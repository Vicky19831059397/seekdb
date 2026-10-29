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
#include "sql/engine/basic/file_scan_utils.h"
#include <charconv>
#include <cmath>
#include <set>

namespace oceanbase { namespace sql { namespace file_sql {

bool valid_utf8(const std::string &text)
{
  size_t i = 0;
  while (i < text.size()) {
    const auto c = static_cast<unsigned char>(text[i++]);
    if (c < 0x80) { if (c == 0) { return false; } continue; }
    uint32_t code = 0;
    size_t count = 0;
    uint32_t minimum = 0;
    if (c >= 0xc2 && c <= 0xdf) { code = c & 0x1f; count = 1; minimum = 0x80; }
    else if (c >= 0xe0 && c <= 0xef) { code = c & 0x0f; count = 2; minimum = 0x800; }
    else if (c >= 0xf0 && c <= 0xf4) { code = c & 7; count = 3; minimum = 0x10000; }
    else { return false; }
    if (text.size() - i < count) { return false; }
    while (count-- > 0) {
      const auto next = static_cast<unsigned char>(text[i++]);
      if ((next & 0xc0) != 0x80) { return false; }
      code = (code << 6) | (next & 0x3f);
    }
    if (code < minimum || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) { return false; }
  }
  return true;
}

static int digits(const std::string &s, size_t start, size_t length)
{
  int value = 0;
  for (size_t i = start; i < start + length; ++i) {
    if (s[i] < '0' || s[i] > '9') { return -1; }
    value = value * 10 + s[i] - '0';
  }
  return value;
}

static bool date_prefix(const std::string &s)
{
  if (s.size() < 10 || s[4] != '-' || s[7] != '-') { return false; }
  const int year = digits(s, 0, 4), month = digits(s, 5, 2), day = digits(s, 8, 2);
  if (year < 1 || month < 1 || month > 12 || day < 1) { return false; }
  const int days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
  const bool leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
  return day <= days[month - 1] + (month == 2 && leap ? 1 : 0);
}

ScalarType infer_scalar(const std::string &text)
{
  if (text == "true" || text == "false" || text == "TRUE" || text == "FALSE") { return ScalarType::Boolean; }
  if (date_prefix(text)) {
    if (text.size() == 10) { return ScalarType::Date; }
    if (text.size() >= 19 && (text[10] == ' ' || text[10] == 'T') && text[13] == ':' && text[16] == ':') {
      const int hour = digits(text, 11, 2), minute = digits(text, 14, 2), second = digits(text, 17, 2);
      bool fraction_ok = text.size() == 19;
      if (text.size() >= 21 && text.size() <= 26 && text[19] == '.') {
        fraction_ok = digits(text, 20, text.size() - 20) >= 0;
      }
      if (hour >= 0 && hour < 24 && minute >= 0 && minute < 60 && second >= 0 && second < 60 && fraction_ok) {
        return ScalarType::Datetime;
      }
    }
  }
  const char *begin = text.data(), *end = begin + text.size();
  if (begin == end) { return ScalarType::Varchar; }
  if (*begin == '+') { ++begin; }
  if (begin == end || (*begin == '-' && begin != text.data())) { return ScalarType::Varchar; }
  int64_t integer = 0;
  auto parsed_int = std::from_chars(begin, end, integer);
  if (parsed_int.ec == std::errc() && parsed_int.ptr == end) { return ScalarType::Bigint; }
  // Out-of-range integer literals stay exact strings; never silently round them.
  bool integer_literal = true;
  for (auto p = begin + (*begin == '-' ? 1 : 0); p != end; ++p) {
    if (*p < '0' || *p > '9') { integer_literal = false; break; }
  }
  if (integer_literal) { return ScalarType::Varchar; }
  double number = 0;
  const auto parsed_double = std::from_chars(begin, end, number, std::chars_format::general);
  if (parsed_double.ec == std::errc() && parsed_double.ptr == end && std::isfinite(number)) { return ScalarType::Double; }
  return ScalarType::Varchar;
}

ScalarType widen(ScalarType left, ScalarType right)
{
  if (left == right || right == ScalarType::Null) { return left; }
  if (left == ScalarType::Null) { return right; }
  if ((left == ScalarType::Bigint && right == ScalarType::Double) ||
      (right == ScalarType::Bigint && left == ScalarType::Double)) { return ScalarType::Double; }
  if ((left == ScalarType::Date && right == ScalarType::Datetime) ||
      (right == ScalarType::Date && left == ScalarType::Datetime)) { return ScalarType::Datetime; }
  return ScalarType::Varchar;
}

bool write_csv_row(std::ostream &output, const Row &row, const CsvOptions &options, ScanError &error)
{
  const auto invalid = [](char c) { return c == 0 || c == '\r' || c == '\n' || static_cast<unsigned char>(c) >= 0x80; };
  if (invalid(options.delimiter) || invalid(options.quote) || invalid(options.escape) ||
      options.delimiter == options.quote || options.delimiter == options.escape ||
      !valid_utf8(options.null_text) || options.null_text.find_first_of(std::string() + options.delimiter + options.quote + '\r' + '\n') != std::string::npos ||
      row.empty() || row.size() > options.max_columns) {
    error.message = "invalid CSV export options or column count"; return false;
  }
  uint64_t bytes = row.size(); // separators plus trailing newline
  for (const auto &field : row) {
    if (!field.is_null && !valid_utf8(field.text)) { error.message = "invalid UTF-8 in CSV export"; return false; }
    bytes += field.is_null ? options.null_text.size() : field.text.size() + 2;
    if (!field.is_null) {
      for (const char c : field.text) { if (c == options.quote || c == options.escape) { ++bytes; } }
    }
    if (bytes > options.max_record_bytes) { error.message = "CSV export record byte limit exceeded"; return false; }
  }
  for (size_t i = 0; i < row.size(); ++i) {
    if (i != 0) { output.put(options.delimiter); }
    if (row[i].is_null) { output << options.null_text; }
    else {
      // Quote every non-NULL field, including an empty string or the NULL token.
      output.put(options.quote);
      for (const char c : row[i].text) {
        if (c == options.quote || c == options.escape) { output.put(options.escape); }
        output.put(c);
      }
      output.put(options.quote);
    }
  }
  output.put('\n');
  if (!output) { error.message = "CSV export write failed"; return false; }
  return true;
}

CsvScanner::CsvScanner(std::istream &input, const CsvOptions &options) : input_(input), options_(options) {}

bool CsvScanner::fail(const char *message, ScanError &error)
{
  failed_ = true;
  error = {message, line_, record_ + 1};
  return false;
}

bool CsvScanner::valid_options(ScanError &error)
{
  const auto invalid = [](char c) { return c == 0 || c == '\r' || c == '\n' || static_cast<unsigned char>(c) >= 0x80; };
  if (invalid(options_.delimiter) || invalid(options_.quote) || invalid(options_.escape) ||
      options_.delimiter == options_.quote || options_.delimiter == options_.escape ||
      options_.sample_size != -1 || options_.max_columns == 0 || options_.max_record_bytes == 0 ||
      options_.max_scan_bytes == 0 || options_.max_batch_rows == 0 || options_.max_batch_bytes == 0 || options_.max_schema_bytes == 0) {
    return fail("invalid CSV options; sample_size must be -1", error);
  }
  return true;
}

bool CsvScanner::reset(ScanError &error)
{
  input_.clear();
  input_.seekg(0);
  line_ = 1; record_ = 0; bytes_ = 0; failed_ = false;
  if (!input_) { return fail("input cannot be rescanned", error); }
  // A UTF-8 BOM is only recognized at the beginning of the stream.
  if (input_.peek() == 0xef) {
    char bom[3] = {};
    input_.read(bom, 3);
    if (input_.gcount() != 3 || static_cast<unsigned char>(bom[1]) != 0xbb ||
        static_cast<unsigned char>(bom[2]) != 0xbf) { return fail("invalid UTF-8 BOM", error); }
    bytes_ = 3;
    if (bytes_ > options_.max_scan_bytes) { return fail("scan byte limit exceeded", error); }
  }
  return true;
}

CsvScanner::ReadStatus CsvScanner::read_record(Row &row, ScanError &error)
{
  row.clear();
  std::string field;
  enum class State { Start, Bare, Quoted, Closed } state = State::Start;
  bool quoted = false, any = false;
  uint64_t record_bytes = 0;
  const auto push = [&]() -> bool {
    if (row.size() >= options_.max_columns) { return fail("column limit exceeded", error); }
    if (!valid_utf8(field)) { return fail("invalid UTF-8 or NUL in CSV field", error); }
    const bool null = !quoted && field == options_.null_text;
    row.push_back({std::move(field), null});
    field.clear(); quoted = false; state = State::Start;
    return true;
  };
  const auto consume = [&]() -> int {
    const int c = input_.get();
    if (c != std::char_traits<char>::eof()) {
      ++bytes_; ++record_bytes;
      if (bytes_ > options_.max_scan_bytes || record_bytes > options_.max_record_bytes) {
        fail("scan or record byte limit exceeded", error);
        return std::char_traits<char>::eof();
      }
    }
    return c;
  };
  for (;;) {
    const int c = consume();
    if (failed_) { return ReadStatus::Error; }
    if (c == std::char_traits<char>::eof()) {
      if (input_.bad() || (!input_.eof() && input_.fail())) { fail("CSV input read failed", error); return ReadStatus::Error; }
      if (state == State::Quoted) { fail("unterminated quoted field", error); return ReadStatus::Error; }
      if (!any) { return ReadStatus::End; }
      if (!push()) { return ReadStatus::Error; }
      ++record_; return ReadStatus::RowReady;
    }
    any = true;
    if (state == State::Quoted) {
      if (c == options_.escape && options_.escape != options_.quote) {
        const int next = consume();
        if (failed_) { return ReadStatus::Error; }
        if (next != options_.quote && next != options_.escape) {
          fail("invalid quoted escape sequence", error); return ReadStatus::Error;
        }
        field += static_cast<char>(next);
      } else if (c == options_.quote) { state = State::Closed; }
      else { field += static_cast<char>(c); if (c == '\n') { ++line_; } }
    } else if (state == State::Closed && c == options_.quote && options_.escape == options_.quote) {
      field += static_cast<char>(c); state = State::Quoted;
    } else if (c == options_.delimiter) {
      if (!push()) { return ReadStatus::Error; }
    } else if (c == '\n' || c == '\r') {
      if (c == '\r' && input_.peek() == '\n') { consume(); }
      if (failed_ || !push()) { return ReadStatus::Error; }
      ++line_; ++record_; return ReadStatus::RowReady;
    } else if (state == State::Start && c == options_.quote) {
      quoted = true; state = State::Quoted;
    } else if (state == State::Closed || c == options_.quote) {
      fail("unexpected character after quote or quote in unquoted field", error); return ReadStatus::Error;
    } else { field += static_cast<char>(c); state = State::Bare; }
  }
}

bool CsvScanner::infer_schema(std::vector<Column> &schema, ScanError &error)
{
  schema.clear(); header_.clear(); inferred_ = false;
  if (!valid_options(error) || !reset(error)) { return false; }
  Row row;
  auto status = read_record(row, error);
  if (status != ReadStatus::RowReady) {
    if (status == ReadStatus::End) { return fail("empty CSV has no schema", error); }
    return false;
  }
  columns_ = row.size();
  std::set<std::string> names;
  uint64_t schema_bytes = 0;
  for (size_t i = 0; i < columns_; ++i) {
    const std::string name = options_.header ? row[i].text : "column" + std::to_string(i + 1);
    schema_bytes += name.size() + sizeof(Column);
    if (schema_bytes > options_.max_schema_bytes) { return fail("CSV schema byte limit exceeded", error); }
    if (name.empty() || !names.insert(name).second) { return fail("empty or duplicate CSV header", error); }
    schema.push_back({name, options_.header || row[i].is_null ? ScalarType::Null : infer_scalar(row[i].text)});
    if (options_.header) { header_.push_back(row[i].text); }
  }
  while ((status = read_record(row, error)) == ReadStatus::RowReady) {
    if (row.size() != columns_) { return fail("CSV field count differs from schema", error); }
    for (size_t i = 0; i < columns_; ++i) {
      if (!row[i].is_null) { schema[i].type = widen(schema[i].type, infer_scalar(row[i].text)); }
    }
  }
  if (status == ReadStatus::Error) { return false; }
  inferred_ = true;
  return rewind(error);
}

bool CsvScanner::rewind(ScanError &error)
{
  if (!inferred_) { return fail("schema inference required before rescan", error); }
  if (!reset(error)) { return false; }
  if (options_.header) {
    Row header;
    if (read_record(header, error) != ReadStatus::RowReady || header.size() != columns_) {
      return fail("CSV header changed", error);
    }
    for (size_t i = 0; i < columns_; ++i) {
      if (header[i].text != header_[i]) { return fail("CSV header changed", error); }
    }
  }
  return true;
}

bool CsvScanner::next_batch(size_t requested, std::vector<Row> &rows, bool &eof, ScanError &error)
{
  rows.clear(); eof = false;
  if (!inferred_ || failed_) { return fail("scanner is not ready; infer schema or rewind first", error); }
  if (requested == 0 || requested > options_.max_batch_rows) { return fail("invalid batch row limit", error); }
  uint64_t batch_bytes = 0;
  while (rows.size() < requested && batch_bytes < options_.max_batch_bytes) {
    Row row;
    const auto status = read_record(row, error);
    if (status == ReadStatus::Error) { rows.clear(); return false; }
    if (status == ReadStatus::End) { eof = true; break; }
    if (row.size() != columns_) { rows.clear(); return fail("CSV field count changed", error); }
    for (const auto &field : row) { batch_bytes += field.text.size() + sizeof(Field); }
    rows.push_back(std::move(row));
  }
  return true;
}

namespace {

class FlatJsonParser
{
public:
  explicit FlatJsonParser(const std::string &text) : text_(text) {}
  bool parse(std::vector<std::pair<std::string, Field>> &object,
             std::vector<ScalarType> &types, size_t max_columns)
  {
    if (!take('{')) { return false; }
    if (take('}')) { return finished(); }
    std::set<std::string> names;
    do {
      std::string name;
      if (object.size() >= max_columns || !string(name) || !names.insert(name).second || !take(':')) { return false; }
      whitespace();
      Field field;
      ScalarType type = ScalarType::Varchar;
      if (pos_ == text_.size()) { return false; }
      if (text_[pos_] == '"') {
        if (!string(field.text)) { return false; }
        const auto inferred = infer_scalar(field.text);
        if (inferred == ScalarType::Date || inferred == ScalarType::Datetime) { type = inferred; }
      } else if (literal("null")) { field.is_null = true; type = ScalarType::Null; }
      else if (literal("true")) { field.text = "true"; type = ScalarType::Boolean; }
      else if (literal("false")) { field.text = "false"; type = ScalarType::Boolean; }
      else {
        if (!number(field.text)) { return false; }
        type = infer_scalar(field.text);
        // Reject numeric overflow instead of converting an invalid JSON number
        // into a string value; do not emit infinities or rounded huge integers.
        if (type != ScalarType::Bigint && type != ScalarType::Double) { return false; }
      }
      object.emplace_back(std::move(name), std::move(field));
      types.push_back(type);
      if (take('}')) { return finished(); }
    } while (take(','));
    return false;
  }
private:
  void whitespace() { while (pos_ < text_.size() && (text_[pos_] == ' ' || text_[pos_] == '\t' || text_[pos_] == '\r')) { ++pos_; } }
  bool finished() { whitespace(); return pos_ == text_.size(); }
  bool take(char c) { whitespace(); if (pos_ < text_.size() && text_[pos_] == c) { ++pos_; return true; } return false; }
  bool literal(const char *s)
  {
    size_t n = 0;
    while (s[n] != 0) { ++n; }
    if (text_.compare(pos_, n, s) != 0) { return false; }
    pos_ += n; return true;
  }
  static bool digit(char c) { return c >= '0' && c <= '9'; }
  bool number(std::string &out)
  {
    const size_t start = pos_;
    if (pos_ < text_.size() && text_[pos_] == '-') { ++pos_; }
    if (pos_ == text_.size() || !digit(text_[pos_])) { return false; }
    if (text_[pos_] == '0') { ++pos_; }
    else { while (pos_ < text_.size() && digit(text_[pos_])) { ++pos_; } }
    if (pos_ < text_.size() && text_[pos_] == '.') {
      ++pos_;
      const size_t fraction = pos_;
      while (pos_ < text_.size() && digit(text_[pos_])) { ++pos_; }
      if (pos_ == fraction) { return false; }
    }
    if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
      ++pos_;
      if (pos_ < text_.size() && (text_[pos_] == '+' || text_[pos_] == '-')) { ++pos_; }
      const size_t exponent = pos_;
      while (pos_ < text_.size() && digit(text_[pos_])) { ++pos_; }
      if (pos_ == exponent) { return false; }
    }
    out = text_.substr(start, pos_ - start); return true;
  }
  bool hex4(uint32_t &code)
  {
    code = 0;
    if (text_.size() - pos_ < 4) { return false; }
    for (int i = 0; i < 4; ++i) {
      const char c = text_[pos_++];
      int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
      if (d < 0) { return false; }
      code = code * 16 + d;
    }
    return true;
  }
  static void utf8(uint32_t code, std::string &out)
  {
    if (code < 0x80) { out += static_cast<char>(code); }
    else if (code < 0x800) {
      out += static_cast<char>(0xc0 | (code >> 6)); out += static_cast<char>(0x80 | (code & 63));
    } else if (code < 0x10000) {
      out += static_cast<char>(0xe0 | (code >> 12)); out += static_cast<char>(0x80 | ((code >> 6) & 63)); out += static_cast<char>(0x80 | (code & 63));
    } else {
      out += static_cast<char>(0xf0 | (code >> 18)); out += static_cast<char>(0x80 | ((code >> 12) & 63));
      out += static_cast<char>(0x80 | ((code >> 6) & 63)); out += static_cast<char>(0x80 | (code & 63));
    }
  }
  bool string(std::string &out)
  {
    if (!take('"')) { return false; }
    while (pos_ < text_.size()) {
      char c = text_[pos_++];
      if (c == '"') { return valid_utf8(out); }
      if (static_cast<unsigned char>(c) < 0x20) { return false; }
      if (c != '\\') { out += c; continue; }
      if (pos_ == text_.size()) { return false; }
      c = text_[pos_++];
      switch (c) {
        case '"': case '\\': case '/': out += c; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case 'n': out += '\n'; break;
        case 'r': out += '\r'; break;
        case 't': out += '\t'; break;
        case 'u': {
          uint32_t code = 0;
          if (!hex4(code)) { return false; }
          if (code >= 0xd800 && code <= 0xdbff) {
            if (text_.size() - pos_ < 6 || text_[pos_] != '\\' || text_[pos_ + 1] != 'u') { return false; }
            pos_ += 2;
            uint32_t low = 0;
            if (!hex4(low) || low < 0xdc00 || low > 0xdfff) { return false; }
            code = 0x10000 + ((code - 0xd800) << 10) + low - 0xdc00;
          } else if (code >= 0xdc00 && code <= 0xdfff) { return false; }
          if (code == 0) { return false; }
          utf8(code, out); break;
        }
        default: return false;
      }
    }
    return false;
  }
  const std::string &text_;
  size_t pos_ = 0;
};

} // namespace

JsonlScanner::JsonlScanner(std::istream &input, const JsonOptions &options) : input_(input), options_(options) {}

bool JsonlScanner::fail(const char *message, ScanError &error)
{
  failed_ = true; error = {message, line_, line_}; return false;
}

bool JsonlScanner::reset(ScanError &error)
{
  input_.clear(); input_.seekg(0); line_ = 0; bytes_ = 0; failed_ = false;
  return input_ ? true : fail("JSONL input cannot be rescanned", error);
}

JsonlScanner::ReadStatus JsonlScanner::read_object(Object &object, std::vector<ScalarType> &types, ScanError &error)
{
  object.clear(); types.clear();
  std::string text;
  for (;;) {
    const int c = input_.get();
    if (c == std::char_traits<char>::eof()) {
      if (input_.bad() || (!input_.eof() && input_.fail())) { fail("JSONL input read failed", error); return ReadStatus::Error; }
      if (text.empty()) { return ReadStatus::End; }
      break;
    }
    ++bytes_;
    if (bytes_ > options_.max_scan_bytes || text.size() >= options_.max_record_bytes) {
      ++line_; fail("JSONL record or scan byte limit exceeded", error); return ReadStatus::Error;
    }
    if (c == '\n') { break; }
    text += static_cast<char>(c);
  }
  ++line_;
  if (line_ == 1 && text.compare(0, 3, "\xef\xbb\xbf") == 0) { text.erase(0,3); }
  if (!valid_utf8(text) || !FlatJsonParser(text).parse(object, types, options_.max_columns)) {
    fail("invalid flat JSON object (nested values, duplicate keys and numeric overflow are rejected)", error);
    return ReadStatus::Error;
  }
  return ReadStatus::RowReady;
}

bool JsonlScanner::infer_schema(std::vector<Column> &schema, ScanError &error)
{
  schema.clear(); schema_.clear(); inferred_ = false;
  if (options_.sample_size != -1 || options_.max_columns == 0 || options_.max_record_bytes == 0 ||
      options_.max_scan_bytes == 0 || options_.max_batch_rows == 0 || options_.max_batch_bytes == 0 || options_.max_schema_bytes == 0) {
    return fail("invalid JSONL options; sample_size must be -1", error);
  }
  if (!reset(error)) { return false; }
  Object object;
  std::vector<ScalarType> types;
  ReadStatus status;
  uint64_t schema_bytes = 0;
  while ((status = read_object(object,types,error)) == ReadStatus::RowReady) {
    for (size_t i = 0; i < object.size(); ++i) {
      size_t index = 0;
      while (index < schema_.size() && schema_[index].name != object[i].first) { ++index; }
      if (index == schema_.size()) {
        if (schema_.size() >= options_.max_columns) { return fail("JSONL union column limit exceeded", error); }
        schema_bytes += object[i].first.size() + sizeof(Column);
        if (schema_bytes > options_.max_schema_bytes) { return fail("JSONL schema byte limit exceeded", error); }
        schema_.push_back({object[i].first, types[i]});
      } else { schema_[index].type = widen(schema_[index].type, types[i]); }
    }
  }
  if (status == ReadStatus::Error) { return false; }
  if (schema_.empty()) { return fail("JSONL contains no columns", error); }
  schema = schema_; inferred_ = true;
  return rewind(error);
}

bool JsonlScanner::rewind(ScanError &error)
{
  if (!inferred_) { return fail("JSONL schema inference required before rescan", error); }
  return reset(error);
}

bool JsonlScanner::next_batch(size_t requested, std::vector<Row> &rows, bool &eof, ScanError &error)
{
  rows.clear(); eof = false;
  if (!inferred_ || failed_) { return fail("JSONL scanner not ready", error); }
  if (requested == 0 || requested > options_.max_batch_rows) { return fail("invalid JSONL batch size", error); }
  Object object;
  std::vector<ScalarType> types;
  uint64_t batch_bytes = 0;
  while (rows.size() < requested && batch_bytes < options_.max_batch_bytes) {
    const auto status = read_object(object,types,error);
    if (status == ReadStatus::End) { eof = true; break; }
    if (status == ReadStatus::Error) { rows.clear(); return false; }
    Row row(schema_.size(), Field{"", true});
    for (auto &entry : object) {
      size_t index = 0;
      while (index < schema_.size() && schema_[index].name != entry.first) { ++index; }
      if (index == schema_.size()) { rows.clear(); return fail("JSONL schema changed after inference", error); }
      row[index] = std::move(entry.second);
    }
    for (const auto &field : row) { batch_bytes += field.text.size() + sizeof(Field); }
    rows.push_back(std::move(row));
  }
  return true;
}

}}} // namespace oceanbase::sql::file_sql
