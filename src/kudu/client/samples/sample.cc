// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include <ctime>
#include <iostream>
#include <sstream>

#include "kudu/client/callbacks.h"
#include "kudu/client/client.h"
#include "kudu/client/row_result.h"
#include "kudu/client/stubs.h"
#include "kudu/client/value.h"
#include "kudu/common/partial_row.h"

using kudu::client::KuduClient;
using kudu::client::KuduClientBuilder;
using kudu::client::KuduColumnSchema;
using kudu::client::KuduError;
using kudu::client::KuduInsert;
using kudu::client::KuduPredicate;
using kudu::client::KuduRowResult;
using kudu::client::KuduScanner;
using kudu::client::KuduSchema;
using kudu::client::KuduSchemaBuilder;
using kudu::client::KuduSession;
using kudu::client::KuduStatusFunctionCallback;
using kudu::client::KuduTable;
using kudu::client::KuduTableAlterer;
using kudu::client::KuduTableCreator;
using kudu::client::KuduValue;
using kudu::client::sp::shared_ptr;
using kudu::KuduPartialRow;
using kudu::MonoDelta;
using kudu::Status;

using std::string;
using std::stringstream;
using std::vector;

static Status CreateClient(const string& addr,
                           shared_ptr<KuduClient>* client) {
  return KuduClientBuilder()
      .add_master_server_addr(addr)
      .default_admin_operation_timeout(MonoDelta::FromSeconds(20))
      .default_rpc_timeout(MonoDelta::FromSeconds(60))
      .Build(client);
}

static KuduSchema CreateSchema() {
  KuduSchema schema;
  KuduSchemaBuilder b;
  b.AddColumn("queue")->Type(KuduColumnSchema::INT32)->NotNull();
  b.AddColumn("op_id_term")->Type(KuduColumnSchema::INT64)->NotNull();
  b.AddColumn("op_id_index")->Type(KuduColumnSchema::INT64)->NotNull();
  b.AddColumn("op_id_offset")->Type(KuduColumnSchema::INT32)->NotNull();
  b.AddColumn("val")->Type(KuduColumnSchema::STRING)->NotNull();
  vector<string> keys;
  keys.push_back("queue");
  keys.push_back("op_id_term");
  keys.push_back("op_id_index");
  keys.push_back("op_id_offset");
  b.SetPrimaryKey(keys);
  KUDU_CHECK_OK(b.Build(&schema));
  return schema;
}

static Status DoesTableExist(const shared_ptr<KuduClient>& client,
                             const string& table_name,
                             bool *exists) {
  shared_ptr<KuduTable> table;
  Status s = client->OpenTable(table_name, &table);
  if (s.ok()) {
    *exists = true;
  } else if (s.IsNotFound()) {
    *exists = false;
    s = Status::OK();
  }
  return s;
}

static Status CreateTable(const shared_ptr<KuduClient>& client,
                          const string& table_name,
                          const KuduSchema& schema,
                          int num_tablets) {
  // Generate the split keys for the table.
  vector<const KuduPartialRow*> splits;
  // int32_t increment = 1000 / num_tablets;
  for (int32_t i = 1; i < num_tablets; i++) {
    KuduPartialRow* row = schema.NewRow();
    KUDU_CHECK_OK(row->SetInt32(0, i));
    splits.push_back(row);
  }

  // Create the table.
  KuduTableCreator* table_creator = client->NewTableCreator();
  Status s = table_creator->table_name(table_name)
      .schema(&schema)
      .num_replicas(3)
      .split_rows(splits)
      .Create();
  delete table_creator;
  return s;
}

static void StatusCB(void* unused, const Status& status) {
  KUDU_LOG(INFO) << "Asynchronous flush finished with status: "
                      << status.ToString();
}

static Status InsertRows(const shared_ptr<KuduTable>& table, int num_rows) {
  shared_ptr<KuduSession> session = table->client()->NewSession();
  KUDU_RETURN_NOT_OK(session->SetFlushMode(KuduSession::MANUAL_FLUSH));
  session->SetTimeoutMillis(60000);

  string val; 
  for (int i = 0; i < 100; i++) {
    val += 'a' + (i % 26);
  }

  KuduStatusFunctionCallback<void*> status_cb(&StatusCB, NULL);

  time_t start = time(NULL);
  for (int i = 0; i < num_rows; i++) {
    KuduInsert* insert = table->NewInsert();
    KuduPartialRow* row = insert->mutable_row();
    KUDU_CHECK_OK(row->SetInt32("queue", i % 6));
    KUDU_CHECK_OK(row->SetInt64("op_id_term", 0));
    KUDU_CHECK_OK(row->SetInt64("op_id_index", 0));
    KUDU_CHECK_OK(row->SetInt32("op_id_offset", 0));
    KUDU_CHECK_OK(row->SetString("val", val));
    KUDU_CHECK_OK(session->Apply(insert));
    if (i % 1024 == 0) {
      session->FlushAsync(&status_cb);
    }
  }
  Status s = session->Flush();
  KUDU_RETURN_NOT_OK(s);

  time_t end = time(NULL);
  KUDU_LOG(INFO) << num_rows << " inserted in " << end - start << " seconds ";

  // Look at the session's errors.
  vector<KuduError*> errors;
  bool overflow;
  session->GetPendingErrors(&errors, &overflow);
  if (errors.size() > 0) {
    s = overflow ? Status::IOError("Overflowed pending errors in session") :
      errors.front()->status();
    while (!errors.empty()) {
      delete errors.back();
      errors.pop_back();
    }
    KUDU_RETURN_NOT_OK(s);
  }

  // Close the session.
  return session->Close();
}

static Status ScanRows(const shared_ptr<KuduTable>& table) {
  const int kLowerBound = 0;
  const int kUpperBound = 999;

  KuduScanner scanner(table.get());

  // Add a predicate: WHERE key >= 5
  /* KuduPredicate* p = table->NewComparisonPredicate(
      "key", KuduPredicate::GREATER_EQUAL, KuduValue::FromInt(kLowerBound));
  KUDU_RETURN_NOT_OK(scanner.AddConjunctPredicate(p));

  // Add a predicate: WHERE key <= 600
  p = table->NewComparisonPredicate(
      "key", KuduPredicate::LESS_EQUAL, KuduValue::FromInt(kUpperBound));
  KUDU_RETURN_NOT_OK(scanner.AddConjunctPredicate(p)); */

  KUDU_RETURN_NOT_OK(scanner.Open());
  vector<KuduRowResult> results;

  int next_row = kLowerBound;
  while (scanner.HasMoreRows()) {
    KUDU_RETURN_NOT_OK(scanner.NextBatch(&results));
    for (vector<KuduRowResult>::iterator iter = results.begin();
        iter != results.end();
        iter++, next_row++) {
      const KuduRowResult& result = *iter;
      int32_t val;
      KUDU_RETURN_NOT_OK(result.GetInt32("queue", &val));
      if (val != next_row) {
        stringstream out;
        out << "Scan returned the wrong results. Expected key "
            << next_row << " but got " << val;
        return Status::IOError(out.str());
      }
      /* int64_t term;
      result.GetInt64("op_id_term", &term);
      int64_t index;
      result.GetInt64("op_id_index", &index);
      int32_t offset;
      result.GetInt32("op_id_offset", &offset);
      KUDU_LOG(INFO) << "OP ID:" << val << ":" << term << ":" << index << ":" << offset; */
    }
    results.clear();
  }

  // next_row is now one past the last row we read.
  int last_row_seen = next_row - 1;

  if (last_row_seen != kUpperBound) {
    stringstream out;
    out << "Scan returned the wrong results. Expected last row to be "
        << kUpperBound << " rows but got " << last_row_seen;
    return Status::IOError(out.str());
  }
  return Status::OK();
}

static void LogCb(void* unused,
                  kudu::client::KuduLogSeverity severity,
                  const char* filename,
                  int line_number,
                  const struct ::tm* time,
                  const char* message,
                  size_t message_len) {
  KUDU_LOG(INFO) << "Received log message from Kudu client library";
  KUDU_LOG(INFO) << " Severity: " << severity;
  KUDU_LOG(INFO) << " Filename: " << filename;
  KUDU_LOG(INFO) << " Line number: " << line_number;
  char time_buf[32];
  // Example: Tue Mar 24 11:46:43 2015.
  KUDU_CHECK(strftime(time_buf, sizeof(time_buf), "%a %b %d %T %Y", time));
  KUDU_LOG(INFO) << " Time: " << time_buf;
  KUDU_LOG(INFO) << " Message: " << string(message, message_len);
}

int main(int argc, char* argv[]) {
  kudu::client::KuduLoggingFunctionCallback<void*> log_cb(&LogCb, NULL);
  kudu::client::InstallLoggingCallback(&log_cb);

  const string kTableName = "test_table";

  // Enable verbose debugging for the client library.
  kudu::client::SetVerboseLogLevel(2);

  // Create and connect a client.
  shared_ptr<KuduClient> client;
  KUDU_CHECK_OK(CreateClient("10.240.0.5", &client));
  KUDU_LOG(INFO) << "Created a client connection";

  // Disable the verbose logging.
  kudu::client::SetVerboseLogLevel(0);

  // Create a schema.
  KuduSchema schema(CreateSchema());
  KUDU_LOG(INFO) << "Created a schema";

  // Create a table with that schema.
  bool exists;
  KUDU_CHECK_OK(DoesTableExist(client, kTableName, &exists));
  if (exists) {
    client->DeleteTable(kTableName);
    KUDU_LOG(INFO) << "Deleting old table before creating new one";
  }
  KUDU_CHECK_OK(CreateTable(client, kTableName, schema, 6));
  KUDU_LOG(INFO) << "Created a table";

  // Insert some rows into the table.
  shared_ptr<KuduTable> table;
  KUDU_CHECK_OK(client->OpenTable(kTableName, &table));
  KUDU_CHECK_OK(InsertRows(table, 1 << 24));
  KUDU_LOG(INFO) << "Inserted some rows into a table";

  // Scan some rows.
  // KUDU_CHECK_OK(ScanRows(table));
  // KUDU_LOG(INFO) << "Scanned some rows out of a table";

  // Delete the table.
  KUDU_CHECK_OK(client->DeleteTable(kTableName));
  KUDU_LOG(INFO) << "Deleted a table";

  // Done!
  KUDU_LOG(INFO) << "Done";
  return 0;
}
