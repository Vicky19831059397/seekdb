/*
 * Copyright (c) 2025 OceanBase.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * http://www.apache.org/licenses/LICENSE-2.0
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#pragma once

#include <cstdint>
#include <istream>
#include <ostream>
#include <string>
#include <vector>

namespace oceanbase { namespace sql { namespace file_sql {

enum class ScalarType { Null, Bigint, Double, Boolean, Varchar, Date, Datetime };
struct ScanError
{
  std::string message;
  uint64_t line = 0;
  uint64_t record = 0;
};
struct CsvOptions
{
  char delimiter = ',';
  char quote = '"';
  char escape = '"';
  bool header = true;
  std::string null_text;
  int64_t sample_size = -1;
  uint64_t max_record_bytes = 8 * 1024 * 1024;
  uint64_t max_scan_bytes = 1024ULL * 1024 * 1024;
  size_t max_columns = 4096;
  size_t max_batch_rows = 4096;
  uint64_t max_batch_bytes = 16 * 1024 * 1024;
  uint64_t max_schema_bytes = 16 * 1024 * 1024;
};
struct Field
{
  std::string text;
  bool is_null = false;
};
using Row = std::vector<Field>;
struct Column
{
  std::string name;
  ScalarType type = ScalarType::Null;
};

bool valid_utf8(const std::string &text);
ScalarType infer_scalar(const std::string &text);
ScalarType widen(ScalarType left, ScalarType right);
// Serialization backend only. COPY must provide an exclusively-created
// temporary output and publish it only after the native query succeeds.
bool write_csv_row(std::ostream &output, const Row &row, const CsvOptions &options, ScanError &error);

// This utility only parses an already-authorized stream. The future SQL entry
// point MUST enforce enable_file_sql, secure_file_priv and file identity checks
// before passing a stream here. It does not open paths or execute queries.
class CsvScanner
{
public:
  CsvScanner(std::istream &input, const CsvOptions &options);
  bool infer_schema(std::vector<Column> &schema, ScanError &error);
  bool rewind(ScanError &error);
  bool next_batch(size_t requested, std::vector<Row> &rows, bool &eof, ScanError &error);
private:
  enum class ReadStatus { RowReady, End, Error };
  ReadStatus read_record(Row &row, ScanError &error);
  bool reset(ScanError &error);
  bool fail(const char *message, ScanError &error);
  bool valid_options(ScanError &error);
  std::istream &input_;
  CsvOptions options_;
  uint64_t line_ = 1;
  uint64_t record_ = 0;
  uint64_t bytes_ = 0;
  size_t columns_ = 0;
  bool inferred_ = false;
  bool failed_ = false;
  std::vector<std::string> header_;
};

struct JsonOptions
{
  int64_t sample_size = -1;
  uint64_t max_record_bytes = 8 * 1024 * 1024;
  uint64_t max_scan_bytes = 1024ULL * 1024 * 1024;
  size_t max_columns = 4096;
  size_t max_batch_rows = 4096;
  uint64_t max_batch_bytes = 16 * 1024 * 1024;
  uint64_t max_schema_bytes = 16 * 1024 * 1024;
};

// Flat JSON objects, one per physical line. Schema is the first-seen ordered
// union over ALL records. Values retain exact decoded strings and number tokens.
class JsonlScanner
{
public:
  JsonlScanner(std::istream &input, const JsonOptions &options);
  bool infer_schema(std::vector<Column> &schema, ScanError &error);
  bool rewind(ScanError &error);
  bool next_batch(size_t requested, std::vector<Row> &rows, bool &eof, ScanError &error);
private:
  using Object = std::vector<std::pair<std::string, Field>>;
  enum class ReadStatus { RowReady, End, Error };
  ReadStatus read_object(Object &object, std::vector<ScalarType> &types, ScanError &error);
  bool fail(const char *message, ScanError &error);
  bool reset(ScanError &error);
  std::istream &input_;
  JsonOptions options_;
  std::vector<Column> schema_;
  uint64_t line_ = 0;
  uint64_t bytes_ = 0;
  bool inferred_ = false;
  bool failed_ = false;
};

}}} // namespace oceanbase::sql::file_sql
