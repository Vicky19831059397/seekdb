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
#pragma once
#include <cstdint>
#include <istream>
#include <memory>
#include <string>
#include "sql/engine/basic/file_scan_utils.h"

namespace oceanbase { namespace sql { namespace file_sql {

// An explicit authorization boundary for one local regular file. secure_root
// must be obtained from the session's secure_file_priv, never from query text.
// The session switch is intentionally false by default. No path-opening API is
// exposed by the scanners themselves.
struct LocalFilePolicy
{
  bool enabled = false;
  std::string secure_root;
  uint64_t max_file_bytes = 1024ULL * 1024 * 1024;
};

class LocalFile
{
public:
  LocalFile();
  ~LocalFile();
  LocalFile(const LocalFile &) = delete;
  LocalFile &operator=(const LocalFile &) = delete;
  bool open(const std::string &path, const LocalFilePolicy &policy, std::string &error);
  bool validate(std::string &error) const;
  void close();
  std::istream *stream();
private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

enum class TextFormat { Csv, Jsonl };

// Query-scoped text scan: authorization and identity checks cannot be skipped
// by callers of next_batch/rescan. Rows are discarded if validation fails.
class TextFileScan
{
public:
  bool open(const std::string &path, const LocalFilePolicy &policy, TextFormat format,
            const CsvOptions &csv, const JsonOptions &json, ScanError &error);
  bool next_batch(size_t requested, std::vector<Row> &rows, bool &eof, ScanError &error);
  bool rescan(ScanError &error);
  void close();
  const std::vector<Column> &schema() const { return schema_; }
private:
  bool validate(ScanError &error);
  // Declaration order ensures scanners die before their input stream.
  LocalFile file_;
  std::unique_ptr<CsvScanner> csv_;
  std::unique_ptr<JsonlScanner> json_;
  std::vector<Column> schema_;
  bool ready_ = false;
};

}}}
