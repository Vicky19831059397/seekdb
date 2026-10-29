/*
 * Copyright (c) 2025 OceanBase.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at http://www.apache.org/licenses/LICENSE-2.0
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "sql/engine/basic/file_scan_utils.h"
#include "sql/engine/basic/file_sql_local_file.h"
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <random>
#include <filesystem>
#include <fstream>
#include <chrono>

using namespace oceanbase::sql::file_sql;
#ifdef FILE_SQL_STANDALONE_TEST
static size_t checks = 0;
#define CHECK(condition) do { ++checks; if (!(condition)) { \
  std::cerr << __LINE__ << ": " << #condition << "\n"; std::exit(1); } } while (0)
#else
#include <gtest/gtest.h>
#define CHECK(condition) ASSERT_TRUE(condition)
#endif

static void scalar_tests()
{
  CHECK(infer_scalar("9223372036854775807") == ScalarType::Bigint);
  CHECK(infer_scalar("-9223372036854775808") == ScalarType::Bigint);
  CHECK(infer_scalar("9223372036854775808") == ScalarType::Varchar);
  CHECK(infer_scalar("-9223372036854775809") == ScalarType::Varchar);
  CHECK(infer_scalar("1.5") == ScalarType::Double);
  CHECK(infer_scalar("+1e3") == ScalarType::Double);
  CHECK(infer_scalar("1e309") == ScalarType::Varchar);
  CHECK(infer_scalar("nan") == ScalarType::Varchar);
  CHECK(infer_scalar("inf") == ScalarType::Varchar);
  CHECK(infer_scalar(" 1") == ScalarType::Varchar);
  CHECK(infer_scalar("+-1") == ScalarType::Varchar);
  CHECK(infer_scalar("true") == ScalarType::Boolean);
  CHECK(infer_scalar("2024-02-29") == ScalarType::Date);
  CHECK(infer_scalar("1900-02-29") == ScalarType::Varchar);
  CHECK(infer_scalar("2000-02-29") == ScalarType::Date);
  CHECK(infer_scalar("2024-04-31") == ScalarType::Varchar);
  CHECK(infer_scalar("0000-01-01") == ScalarType::Varchar);
  CHECK(infer_scalar("2024-01-01 23:59:59.123456") == ScalarType::Datetime);
  CHECK(infer_scalar("2024-01-01T24:00:00") == ScalarType::Varchar);
  CHECK(infer_scalar("2024-01-01T00:00:00.") == ScalarType::Varchar);
  CHECK(widen(ScalarType::Null, ScalarType::Boolean) == ScalarType::Boolean);
  CHECK(widen(ScalarType::Bigint, ScalarType::Double) == ScalarType::Double);
  CHECK(widen(ScalarType::Date, ScalarType::Datetime) == ScalarType::Datetime);
  CHECK(widen(ScalarType::Boolean, ScalarType::Bigint) == ScalarType::Varchar);
  for (auto a : {ScalarType::Null, ScalarType::Bigint, ScalarType::Double,
                 ScalarType::Boolean, ScalarType::Varchar, ScalarType::Date, ScalarType::Datetime}) {
    for (auto b : {ScalarType::Null, ScalarType::Bigint, ScalarType::Double,
                   ScalarType::Boolean, ScalarType::Varchar, ScalarType::Date, ScalarType::Datetime}) {
      CHECK(widen(a,b) == widen(b,a));
    }
  }
  CHECK(valid_utf8("中文"));
  CHECK(!valid_utf8(std::string("a\0b",3)));
  CHECK(!valid_utf8("\xc0\xaf"));
  CHECK(!valid_utf8("\xed\xa0\x80"));
  CHECK(!valid_utf8("\xf4\x90\x80\x80"));
}

static void csv_tests()
{
  std::istringstream input("\xef\xbb\xbf" "id,text,empty,date\r\n1,\"中文,\"\"引号\"\"\r\n第二行\",,2024-01-01\r\n2,\"\",,2024-01-02 12:00:00");
  CsvOptions options;
  CsvScanner scanner(input, options);
  ScanError error;
  std::vector<Column> schema;
  CHECK(scanner.infer_schema(schema,error));
  CHECK(schema.size() == 4);
  CHECK(schema[0].type == ScalarType::Bigint);
  CHECK(schema[2].type == ScalarType::Null);
  CHECK(schema[3].type == ScalarType::Datetime);
  std::vector<Row> rows;
  bool eof = false;
  CHECK(scanner.next_batch(1,rows,eof,error));
  CHECK(rows.size() == 1);
  CHECK(rows[0][1].text == "中文,\"引号\"\r\n第二行");
  CHECK(rows[0][2].is_null);
  CHECK(scanner.next_batch(3,rows,eof,error));
  CHECK(rows.size() == 1 && eof);
  CHECK(rows[0][1].text.empty() && !rows[0][1].is_null);
  CHECK(scanner.rewind(error));
  CHECK(scanner.next_batch(4,rows,eof,error));
  CHECK(rows.size() == 2 && eof);
  CHECK(scanner.next_batch(4,rows,eof,error) && rows.empty() && eof);

  for (const std::string &bad : std::vector<std::string>{"a,a\n1,2", "a,\n1,2", "a,b\n1", "a,b\n1,2,3",
       "a\n\"unfinished", "a\n\"x\"y", "a\nx\"y", std::string("a\n\xff")}) {
    std::istringstream broken(bad);
    CsvScanner parser(broken,options);
    CHECK(!parser.infer_schema(schema,error));
    CHECK(!error.message.empty());
    CHECK(!parser.next_batch(1,rows,eof,error));
  }
  {
    std::istringstream custom("1;\\N;\"a\\\"b\"\n2;3;xyz\n");
    CsvOptions opts; opts.header=false; opts.delimiter=';'; opts.escape='\\'; opts.null_text="\\N";
    CsvScanner parser(custom,opts);
    CHECK(parser.infer_schema(schema,error));
    CHECK(schema[0].name == "column1");
    CHECK(parser.next_batch(5,rows,eof,error));
    CHECK(rows[0][1].is_null && rows[0][2].text == "a\"b");
  }
  {
    std::istringstream tiny("a\n123456789\n");
    CsvOptions opts; opts.max_record_bytes=5;
    CsvScanner parser(tiny,opts);
    CHECK(!parser.infer_schema(schema,error));
  }
  {
    std::istringstream tiny("a\n1\n2\n");
    CsvOptions opts; opts.sample_size=1;
    CsvScanner parser(tiny,opts);
    CHECK(!parser.infer_schema(schema,error));
  }
  {
    std::istringstream empty("");
    CsvScanner parser(empty,options);
    CHECK(!parser.infer_schema(schema,error));
  }
  {
    std::istringstream late("x\n1\n2\n3\nlate text\n");
    CsvScanner parser(late,options);
    CHECK(parser.infer_schema(schema,error));
    CHECK(schema[0].type == ScalarType::Varchar);
  }
  {
    std::istringstream input2("a,b\n1,2\n3,4\n");
    CsvOptions opts; opts.max_batch_bytes=1;
    CsvScanner parser(input2,opts);
    CHECK(parser.infer_schema(schema,error));
    CHECK(parser.next_batch(10,rows,eof,error) && rows.size() == 1);
  }
}

static void json_tests()
{
  std::istringstream input("{\"a\":1,\"name\":\"中文\",\"n\":null}\r\n{\"b\":true,\"a\":2.5}\n{\"name\":\"\\uD83D\\uDE00\",\"a\":\"text\"}");
  JsonOptions options;
  JsonlScanner scanner(input,options);
  std::vector<Column> schema;
  ScanError error;
  CHECK(scanner.infer_schema(schema,error));
  CHECK(schema.size() == 4);
  CHECK(schema[0].name == "a" && schema[0].type == ScalarType::Varchar);
  CHECK(schema[1].name == "name" && schema[1].type == ScalarType::Varchar);
  CHECK(schema[2].name == "n" && schema[2].type == ScalarType::Null);
  CHECK(schema[3].name == "b" && schema[3].type == ScalarType::Boolean);
  std::vector<Row> rows;
  bool eof = false;
  CHECK(scanner.next_batch(4,rows,eof,error));
  CHECK(rows.size() == 3 && eof);
  CHECK(rows[0][3].is_null && rows[1][1].is_null);
  CHECK(rows[1][0].text == "2.5");
  CHECK(rows[2][1].text == "\xf0\x9f\x98\x80");
  CHECK(scanner.rewind(error));
  CHECK(scanner.next_batch(1,rows,eof,error) && rows.size() == 1);
  for (const std::string &bad : std::vector<std::string>{
       "{\"a\":{}}", "{\"a\":[]}", "[]", "{\"a\":1,\"a\":2}",
       "{\"a\":01}", "{\"a\":+1}", "{\"a\":1.}", "{\"a\":NaN}",
       "{\"a\":1e309}", "{\"a\":9223372036854775808}", "{\"a\":1,}",
       "{\"a\":\"\\uD800\"}", "{\"a\":\"\\uDC00\"}", "{\"a\":\"\\u0000\"}",
       "{\"a\":true} garbage", "\n", "{\"a\":\"\xff\"}"}) {
    std::istringstream broken(bad);
    JsonlScanner parser(broken,options);
    CHECK(!parser.infer_schema(schema,error));
    CHECK(!error.message.empty());
  }
  {
    std::istringstream text("{\"a\":1}\n{\"b\":2}");
    JsonOptions opts; opts.max_columns=1;
    JsonlScanner parser(text,opts);
    CHECK(!parser.infer_schema(schema,error));
  }
  {
    std::istringstream text("{\"a\":\"123\",\"b\":\"true\",\"c\":\"2024-02-29\"}");
    JsonlScanner parser(text,options);
    CHECK(parser.infer_schema(schema,error));
    CHECK(schema[0].type == ScalarType::Varchar && schema[1].type == ScalarType::Varchar);
    CHECK(schema[2].type == ScalarType::Date);
  }
  {
    std::istringstream text("{\"a\":1}\n{\"a\":2.5}");
    JsonlScanner parser(text,options);
    CHECK(parser.infer_schema(schema,error));
    CHECK(schema[0].type == ScalarType::Double);
  }
}

static void export_tests()
{
  for (const char escape : {'"', '\\'}) {
    CsvOptions opts; opts.header=false; opts.null_text="NULL"; opts.escape=escape;
    const Row original = {{"",false}, {"",true}, {"NULL",false}, {"中文,\"x\"\n\\",false}};
    std::ostringstream out;
    ScanError error;
    CHECK(write_csv_row(out,original,opts,error));
    std::istringstream in(out.str());
    CsvScanner parser(in,opts);
    std::vector<Column> schema;
    CHECK(parser.infer_schema(schema,error));
    std::vector<Row> rows;
    bool eof=false;
    CHECK(parser.next_batch(2,rows,eof,error));
    CHECK(rows.size() == 1 && eof);
    for (size_t i=0; i<original.size(); ++i) {
      CHECK(original[i].is_null || rows[0][i].text == original[i].text);
      CHECK(rows[0][i].is_null == original[i].is_null);
    }
  }
  CsvOptions options;
  ScanError error;
  std::ostringstream out;
  const Row null_row = {{"",true}};
  const Row text_row = {{"abc",false}};
  options.null_text="bad,null";
  CHECK(!write_csv_row(out,null_row,options,error) && out.str().empty());
  options.null_text=""; options.max_record_bytes=3;
  CHECK(!write_csv_row(out,text_row,options,error) && out.str().empty());
  out.setstate(std::ios::badbit);
  options.max_record_bytes=100;
  CHECK(!write_csv_row(out,text_row,options,error));
}

static void randomized_roundtrip_tests()
{
  std::mt19937 rng(20261029);
  const std::vector<std::string> alphabet = {"a", "0", ",", "\"", "\\", "\n", "\r", " ", "中文", "\xf0\x9f\x98\x80"};
  for (const char escape : {'"', '\\'}) {
    for (int trial=0; trial<200; ++trial) {
      CsvOptions options; options.header=false; options.escape=escape; options.null_text="NULL";
      std::vector<Row> expected;
      std::ostringstream output;
      ScanError error;
      const size_t width = 1 + rng()%8;
      const size_t count = 1 + rng()%12;
      for (size_t r=0; r<count; ++r) {
        Row row;
        for (size_t c=0; c<width; ++c) {
          Field field; field.is_null=rng()%7==0;
          if (!field.is_null) {
            const size_t length = rng()%20;
            for (size_t i=0; i<length; ++i) { field.text += alphabet[rng()%alphabet.size()]; }
          }
          row.push_back(std::move(field));
        }
        CHECK(write_csv_row(output,row,options,error));
        expected.push_back(std::move(row));
      }
      std::istringstream input(output.str());
      CsvScanner scanner(input,options);
      std::vector<Column> schema;
      CHECK(scanner.infer_schema(schema,error));
      CHECK(schema.size() == width);
      size_t offset=0;
      bool eof=false;
      while (!eof) {
        std::vector<Row> rows;
        CHECK(scanner.next_batch(1+rng()%5,rows,eof,error));
        for (const auto &row : rows) {
          CHECK(offset < expected.size());
          CHECK(row.size() == expected[offset].size());
          for (size_t c=0; c<width; ++c) {
            CHECK(row[c].is_null == expected[offset][c].is_null);
            CHECK(row[c].is_null || row[c].text == expected[offset][c].text);
          }
          ++offset;
        }
      }
      CHECK(offset == count);
    }
  }
}

static void local_file_tests()
{
  namespace fs = std::filesystem;
  const auto path_text = [](const fs::path &path) {
    const auto utf8 = path.u8string();
    return std::string(reinterpret_cast<const char *>(utf8.data()),utf8.size());
  };
  const auto root = fs::temp_directory_path() / ("file-sql-test-" + std::to_string(std::chrono::high_resolution_clock::now().time_since_epoch().count()));
  CHECK(fs::create_directory(root));
  CHECK(fs::create_directory(root / "allowed"));
  const auto inside = root / "allowed" / "data.csv";
  const auto outside = root / "outside.csv";
  { std::ofstream out(inside); out << "id,name\n1,hello\n2,world\n"; }
  { std::ofstream out(outside); out << "secret"; }
  LocalFilePolicy policy;
  policy.secure_root = path_text(root / "allowed");
  LocalFile file;
  std::string error;
  CHECK(!file.open(path_text(inside),policy,error));
  policy.enabled = true;
  CHECK(!file.open(path_text(outside),policy,error));
  CHECK(!file.open(path_text(root / "allowed" / ".." / "outside.csv"),policy,error));
  CHECK(!file.open(path_text(root / "allowed"),policy,error));
  CHECK(file.open(path_text(inside),policy,error));
  CHECK(file.validate(error));
  CsvOptions options;
  CsvScanner scanner(*file.stream(),options);
  ScanError scan_error;
  std::vector<Column> schema;
  CHECK(scanner.infer_schema(schema,scan_error));
  std::vector<Row> rows;
  bool eof=false;
  CHECK(scanner.next_batch(4,rows,eof,scan_error));
  CHECK(file.validate(error));
  CHECK(rows.size() == 2 && eof && rows[1][1].text == "world");
  CHECK(scanner.rewind(scan_error));
  CHECK(scanner.next_batch(1,rows,eof,scan_error));
  CHECK(rows[0][0].text == "1");
  {
    // Windows must reject a new writer while the query owns its read handle;
    // POSIX instead detects the mutation before exposing another batch.
    std::ofstream writer(inside,std::ios::app);
#ifdef _WIN32
    CHECK(!writer.is_open());
#else
    CHECK(writer.is_open()); writer << "3,changed\n"; writer.close();
    CHECK(!file.validate(error));
#endif
  }
  file.close();
  CHECK(file.stream() == nullptr);
  policy.max_file_bytes = 1;
  CHECK(!file.open(path_text(inside),policy,error));
  policy.max_file_bytes = 1024;
  {
    TextFileScan scan;
    CHECK(scan.open(path_text(inside),policy,TextFormat::Csv,CsvOptions{},JsonOptions{},scan_error));
    CHECK(scan.schema().size() == 2);
    CHECK(scan.next_batch(1,rows,eof,scan_error));
    CHECK(rows[0][0].text == "1");
    CHECK(scan.rescan(scan_error));
    CHECK(scan.next_batch(1,rows,eof,scan_error) && rows[0][0].text == "1");
    scan.close();
    CHECK(!scan.next_batch(1,rows,eof,scan_error) && rows.empty());
    LocalFilePolicy denied=policy; denied.enabled=false;
    CHECK(!scan.open(path_text(inside),denied,TextFormat::Csv,CsvOptions{},JsonOptions{},scan_error));
  }
  std::error_code ec;
  fs::create_symlink(outside,root / "allowed" / "escape.csv",ec);
  if (!ec) { CHECK(!file.open(path_text(root / "allowed" / "escape.csv"),policy,error)); }
  else { std::cout << "SKIP: OS did not permit creating a symlink fixture\n"; }
  // Only remove fixtures under the unique test-owned temporary directory.
  fs::remove_all(root);
}

#ifdef FILE_SQL_STANDALONE_TEST
int main()
{
  scalar_tests(); csv_tests(); json_tests(); export_tests(); randomized_roundtrip_tests();
  local_file_tests();
  std::cout << checks << " file scan utility checks passed\n";
}
#else
TEST(FileScanUtils, ScalarTypes) { scalar_tests(); }
TEST(FileScanUtils, Csv) { csv_tests(); }
TEST(FileScanUtils, Jsonl) { json_tests(); }
TEST(FileScanUtils, CsvExport) { export_tests(); }
TEST(FileScanUtils, RandomizedRoundtrip) { randomized_roundtrip_tests(); }
TEST(FileScanUtils, AuthorizedLocalFile) { local_file_tests(); }
int main(int argc, char **argv)
{
  ::testing::InitGoogleTest(&argc,argv);
  return RUN_ALL_TESTS();
}
#endif
