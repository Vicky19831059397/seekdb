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
#include "sql/engine/basic/file_sql_local_file.h"
#include "sql/engine/basic/file_scan_utils.h"
#include <array>
#include <filesystem>
#include <limits>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace oceanbase { namespace sql { namespace file_sql {
namespace {
#ifdef _WIN32
using Handle = HANDLE;
static Handle invalid_handle() { return INVALID_HANDLE_VALUE; }
static void close_handle(Handle h) { if (h != invalid_handle()) { CloseHandle(h); } }
static std::wstring final_path(Handle h)
{
  const DWORD needed = GetFinalPathNameByHandleW(h, nullptr, 0, FILE_NAME_NORMALIZED);
  if (needed == 0 || needed > 32768) { return {}; }
  std::wstring path(needed, L'\0');
  const DWORD length = GetFinalPathNameByHandleW(h, &path[0], needed, FILE_NAME_NORMALIZED);
  if (length == 0 || length >= needed) { return {}; }
  path.resize(length);
  return path;
}
static std::filesystem::path utf8_path(const std::string &path)
{
#if defined(__cpp_char8_t)
  return std::filesystem::path(std::u8string(path.begin(),path.end()));
#else
  return std::filesystem::u8path(path);
#endif
}
static bool path_within(const std::wstring &file, std::wstring root)
{
  while (!root.empty() && root.back() == L'\\') { root.pop_back(); }
  return file.size() > root.size() && file[root.size()] == L'\\' &&
    // Fail closed for case-sensitive Windows directories: do not authorize a
    // differently-cased sibling just because most Windows volumes ignore case.
    CompareStringOrdinal(file.data(), static_cast<int>(root.size()), root.data(), static_cast<int>(root.size()), FALSE) == CSTR_EQUAL;
}
#else
using Handle = int;
static Handle invalid_handle() { return -1; }
static void close_handle(Handle h) { if (h != -1) { ::close(h); } }
#endif

class HandleBuffer : public std::streambuf
{
public:
  explicit HandleBuffer(Handle handle) : handle_(handle) { setg(buffer_.data(), buffer_.data(), buffer_.data()); }
  bool io_error() const { return io_error_; }
protected:
  int_type underflow() override
  {
    if (gptr() < egptr()) { return traits_type::to_int_type(*gptr()); }
#ifdef _WIN32
    DWORD count = 0;
    if (!ReadFile(handle_, buffer_.data(), static_cast<DWORD>(buffer_.size()), &count, nullptr)) {
      io_error_ = true; return traits_type::eof();
    }
#else
    ssize_t count;
    do { count = ::read(handle_, buffer_.data(), buffer_.size()); } while (count < 0 && errno == EINTR);
    if (count < 0) { io_error_ = true; return traits_type::eof(); }
#endif
    if (count == 0) { return traits_type::eof(); }
    setg(buffer_.data(), buffer_.data(), buffer_.data() + count);
    return traits_type::to_int_type(*gptr());
  }
  pos_type seekoff(off_type offset, std::ios_base::seekdir direction, std::ios_base::openmode which) override
  {
    if ((which & std::ios_base::out) != 0 || (which & std::ios_base::in) == 0) { return pos_type(off_type(-1)); }
    if (direction == std::ios_base::cur) { offset -= egptr() - gptr(); }
#ifdef _WIN32
    LARGE_INTEGER distance, position;
    distance.QuadPart = offset;
    DWORD method = direction == std::ios_base::beg ? FILE_BEGIN : direction == std::ios_base::cur ? FILE_CURRENT : FILE_END;
    if (!SetFilePointerEx(handle_, distance, &position, method)) { return pos_type(off_type(-1)); }
    const auto value = position.QuadPart;
#else
    const int method = direction == std::ios_base::beg ? SEEK_SET : direction == std::ios_base::cur ? SEEK_CUR : SEEK_END;
    const auto value = ::lseek(handle_, offset, method);
    if (value < 0) { return pos_type(off_type(-1)); }
#endif
    setg(buffer_.data(), buffer_.data(), buffer_.data());
    io_error_ = false;
    return pos_type(value);
  }
  pos_type seekpos(pos_type position, std::ios_base::openmode which) override
  {
    return seekoff(off_type(position), std::ios_base::beg, which);
  }
private:
  Handle handle_;
  bool io_error_ = false;
  std::array<char, 64 * 1024> buffer_;
};
} // namespace

class LocalFile::Impl
{
public:
  ~Impl() { close_handle(handle); }
  Handle handle = invalid_handle();
  std::unique_ptr<HandleBuffer> buffer;
  std::unique_ptr<std::istream> input;
  uint64_t size = 0;
#ifdef _WIN32
  BY_HANDLE_FILE_INFORMATION original{};
#else
  struct stat original{};
#endif
};

LocalFile::LocalFile() = default;
LocalFile::~LocalFile() = default;
void LocalFile::close() { impl_.reset(); }
std::istream *LocalFile::stream() { return impl_ ? impl_->input.get() : nullptr; }

bool LocalFile::open(const std::string &path, const LocalFilePolicy &policy, std::string &error)
{
  close();
  if (!policy.enabled) { error = "file SQL is disabled"; return false; }
  if (path.empty() || policy.secure_root.empty() || !valid_utf8(path) || !valid_utf8(policy.secure_root) ||
      path.find("://") != std::string::npos || policy.max_file_bytes == 0) {
    error = "a nonempty secure_file_priv directory and local path are required"; return false;
  }
  auto file = std::make_unique<Impl>();
#ifdef _WIN32
  const auto file_path = utf8_path(path);
  const auto root_path = utf8_path(policy.secure_root);
  if (path.rfind("\\\\",0) == 0 || path.rfind("//",0) == 0 ||
      policy.secure_root.rfind("\\\\",0) == 0 || policy.secure_root.rfind("//",0) == 0 ||
      GetDriveTypeW(root_path.root_path().c_str()) == DRIVE_REMOTE) {
    error = "remote and device paths are not supported"; return false;
  }
  Handle root = CreateFileW(root_path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  if (root == invalid_handle()) { error = "cannot open secure_file_priv directory"; return false; }
  BY_HANDLE_FILE_INFORMATION root_info{};
  const bool is_directory = GetFileInformationByHandle(root, &root_info) && (root_info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
  const auto resolved_root = final_path(root);
  close_handle(root);
  if (!is_directory || resolved_root.empty() || resolved_root.rfind(L"\\\\?\\UNC\\",0) == 0) {
    error = "invalid or remote secure_file_priv directory"; return false;
  }
  // Denying write/delete sharing also rejects existing writers, preventing
  // concurrent mutation on Windows while the query owns this handle.
  file->handle = CreateFileW(file_path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                            FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
  if (file->handle == invalid_handle()) { error = "cannot open file read-only, or file has an active writer"; return false; }
  const auto resolved_file = final_path(file->handle);
  if (resolved_file.empty() || !path_within(resolved_file, resolved_root) || GetFileType(file->handle) != FILE_TYPE_DISK ||
      !GetFileInformationByHandle(file->handle, &file->original) ||
      (file->original.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
    error = "file is not a regular file inside secure_file_priv"; return false;
  }
  file->size = (uint64_t(file->original.nFileSizeHigh) << 32) | file->original.nFileSizeLow;
#else
  std::error_code ec;
  const auto root = std::filesystem::canonical(policy.secure_root, ec);
  if (ec || !std::filesystem::is_directory(root, ec) || ec) { error = "invalid secure_file_priv directory"; return false; }
  // O_NONBLOCK prevents a FIFO from blocking before regular-file validation.
  file->handle = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
  if (file->handle == -1 || ::fstat(file->handle, &file->original) != 0 || !S_ISREG(file->original.st_mode)) {
    error = "cannot open a regular local file"; return false;
  }
#if defined(__linux__)
  const auto actual = std::filesystem::canonical("/proc/self/fd/" + std::to_string(file->handle), ec);
#elif defined(__APPLE__)
  char actual_path[4096];
  if (::fcntl(file->handle, F_GETPATH, actual_path) == -1) { error = "cannot resolve opened file"; return false; }
  const auto actual = std::filesystem::canonical(actual_path, ec);
#else
  error = "opened-handle path verification unsupported on this platform"; return false;
#endif
#if defined(__linux__) || defined(__APPLE__)
  if (ec) { error = "cannot resolve opened file"; return false; }
  auto a = actual.begin();
  for (auto r = root.begin(); r != root.end(); ++r, ++a) {
    if (a == actual.end() || *a != *r) { error = "file escapes secure_file_priv"; return false; }
  }
  if (a == actual.end()) { error = "secure_file_priv directory is not a file"; return false; }
#endif
  if (file->original.st_size < 0) { error = "invalid file size"; return false; }
  file->size = static_cast<uint64_t>(file->original.st_size);
#endif
  if (file->size > policy.max_file_bytes) { error = "file size limit exceeded"; return false; }
  file->buffer = std::make_unique<HandleBuffer>(file->handle);
  file->input = std::make_unique<std::istream>(file->buffer.get());
  impl_ = std::move(file);
  return true;
}

bool LocalFile::validate(std::string &error) const
{
  if (!impl_) { error = "no open local file"; return false; }
  if (impl_->buffer->io_error()) { error = "local file read error"; return false; }
#ifdef _WIN32
  BY_HANDLE_FILE_INFORMATION now{};
  const auto &before = impl_->original;
  if (!GetFileInformationByHandle(impl_->handle, &now) || now.dwVolumeSerialNumber != before.dwVolumeSerialNumber ||
      now.nFileIndexHigh != before.nFileIndexHigh || now.nFileIndexLow != before.nFileIndexLow ||
      now.nFileSizeHigh != before.nFileSizeHigh || now.nFileSizeLow != before.nFileSizeLow ||
      CompareFileTime(&now.ftLastWriteTime, &before.ftLastWriteTime) != 0) {
    error = "local file changed during query"; return false;
  }
#else
  struct stat now{};
  const auto &before = impl_->original;
  if (::fstat(impl_->handle, &now) != 0 || now.st_dev != before.st_dev || now.st_ino != before.st_ino ||
      now.st_size != before.st_size || now.st_mtime != before.st_mtime || now.st_ctime != before.st_ctime) {
    error = "local file changed during query"; return false;
  }
#if defined(__linux__)
  if (now.st_mtim.tv_nsec != before.st_mtim.tv_nsec || now.st_ctim.tv_nsec != before.st_ctim.tv_nsec) {
    error = "local file changed during query"; return false;
  }
#elif defined(__APPLE__)
  if (now.st_mtimespec.tv_nsec != before.st_mtimespec.tv_nsec || now.st_ctimespec.tv_nsec != before.st_ctimespec.tv_nsec) {
    error = "local file changed during query"; return false;
  }
#endif
#endif
  return true;
}

void TextFileScan::close()
{
  ready_ = false; csv_.reset(); json_.reset(); schema_.clear(); file_.close();
}

bool TextFileScan::validate(ScanError &error)
{
  std::string message;
  if (!file_.validate(message)) { ready_ = false; error.message = message; return false; }
  return true;
}

bool TextFileScan::open(const std::string &path, const LocalFilePolicy &policy, TextFormat format,
                        const CsvOptions &csv, const JsonOptions &json, ScanError &error)
{
  close();
  std::string message;
  if (!file_.open(path,policy,message)) { error.message = message; return false; }
  bool ok = false;
  if (format == TextFormat::Csv) {
    csv_ = std::make_unique<CsvScanner>(*file_.stream(),csv);
    ok = csv_->infer_schema(schema_,error);
  } else {
    json_ = std::make_unique<JsonlScanner>(*file_.stream(),json);
    ok = json_->infer_schema(schema_,error);
  }
  if (!ok || !validate(error)) { close(); return false; }
  ready_ = true;
  return true;
}

bool TextFileScan::next_batch(size_t requested, std::vector<Row> &rows, bool &eof, ScanError &error)
{
  rows.clear(); eof = false;
  if (!ready_) { error.message = "file scan is not open"; return false; }
  if (!validate(error)) { return false; }
  const bool ok = csv_ ? csv_->next_batch(requested,rows,eof,error) : json_->next_batch(requested,rows,eof,error);
  if (!ok || !validate(error)) { rows.clear(); ready_ = false; return false; }
  return true;
}

bool TextFileScan::rescan(ScanError &error)
{
  if (!ready_) { error.message = "file scan is not open"; return false; }
  if (!validate(error)) { return false; }
  const bool ok = csv_ ? csv_->rewind(error) : json_->rewind(error);
  if (!ok || !validate(error)) { ready_ = false; return false; }
  return true;
}

}}}
