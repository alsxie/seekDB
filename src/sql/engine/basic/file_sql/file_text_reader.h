/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// 作者：FDE WorkMate
//
// Streaming readers for line oriented text files. Memory is bounded by the
// read block plus the largest record (max_line_size), never by the file.

#pragma once

#include "sql/engine/basic/file_sql/file_sql_common.h"

namespace oceanbase
{
namespace sql
{
namespace file_sql
{

static const int64_t FILE_SQL_ERROR_MSG_LEN = 256;

// Buffered sequential byte input over a read only file descriptor.
class BufferedFileInput
{
public:
  BufferedFileInput() : fd_(-1), buf_(), pos_(0), len_(0), offset_(0), path_(), fingerprint_(),
                        reached_eof_(false) {}
  ~BufferedFileInput() { close(); }
  // `expect` (optional) must match the opened file.
  int open(const common::ObString &path, const FileFingerprint *expect);
  // Restarts from the beginning of the same file version.
  int rewind();
  void close();
  OB_INLINE int next(char &c, bool &eof)
  {
    int ret = common::OB_SUCCESS;
    if (OB_LIKELY(pos_ < len_)) {
      c = buf_.ptr()[pos_++];
      eof = false;
    } else {
      ret = fill(c, eof);
    }
    return ret;
  }
  const FileFingerprint &get_fingerprint() const { return fingerprint_; }
  const common::ObString &get_path() const { return path_; }
  bool is_open() const { return fd_ >= 0; }
private:
  int fill(char &c, bool &eof);
  int skip_bom();
  DISALLOW_COPY_AND_ASSIGN(BufferedFileInput);
  int fd_;
  FileBuffer buf_;
  int64_t pos_;
  int64_t len_;
  int64_t offset_;
  common::ObString path_;
  FileFingerprint fingerprint_;
  bool reached_eof_;
  char path_buf_[PATH_MAX + 1];
};

// RFC 4180 CSV record reader: configurable delimiter / quote / escape, quoted
// fields may contain delimiters and line breaks, LF and CRLF line endings,
// UTF-8 BOM, completely blank lines are skipped.
//
// Malformed records are returned as OB_INVALID_DATA with get_error_msg()
// describing the problem; the reader is positioned after the bad record so a
// caller may choose to skip it (ignore_errors).
class CsvRecordReader
{
public:
  CsvRecordReader() : input_(), opt_(), max_line_size_(FILE_SQL_DEFAULT_MAX_LINE_SIZE), record_(),
                      fields_(), line_no_(1), record_line_(0), recoverable_(true)
  { error_msg_[0] = '\0'; }
  int open(const common::ObString &path, const FileFingerprint *expect, const CsvOptions &opt,
           const int64_t max_line_size);
  int rewind();
  void close() { input_.close(); }
  int next_record(bool &has_record);
  int64_t get_field_count() const { return fields_.count(); }
  common::ObString get_field(const int64_t idx) const
  {
    const FieldPos &f = fields_.at(idx);
    return common::ObString(static_cast<int32_t>(f.len_), record_.ptr() + f.start_);
  }
  bool is_field_quoted(const int64_t idx) const { return fields_.at(idx).quoted_; }
  int64_t get_record_line() const { return record_line_; }
  const char *get_error_msg() const { return error_msg_; }
  // false when the last error left the reader unable to continue (EOF inside quotes)
  bool is_recoverable() const { return recoverable_; }
  const FileFingerprint &get_fingerprint() const { return input_.get_fingerprint(); }
private:
  struct FieldPos
  {
    FieldPos() : start_(0), len_(0), quoted_(false) {}
    FieldPos(int64_t start, int64_t len, bool quoted) : start_(start), len_(len), quoted_(quoted) {}
    TO_STRING_KV(K_(start), K_(len), K_(quoted));
    int64_t start_;
    int64_t len_;
    bool quoted_;
  };
  int end_field(const int64_t start, const bool quoted, const bool strip_cr);
  int skip_to_line_end();
  int set_error(const char *fmt, ...);
  DISALLOW_COPY_AND_ASSIGN(CsvRecordReader);
  BufferedFileInput input_;
  CsvOptions opt_;
  int64_t max_line_size_;
  FileBuffer record_;
  common::ObSEArray<FieldPos, 32> fields_;
  int64_t line_no_;
  int64_t record_line_;
  bool recoverable_;
  char error_msg_[FILE_SQL_ERROR_MSG_LEN];
};

enum class JsonScalarType : int8_t
{
  JSON_NULL = 0,
  JSON_BOOL = 1,
  JSON_NUMBER = 2,
  JSON_STRING = 3,
};

struct JsonMember
{
  JsonMember() : key_(), type_(JsonScalarType::JSON_NULL), bool_(false), text_() {}
  TO_STRING_KV(K_(key), K_(type), K_(bool), K_(text));
  common::ObString key_;
  JsonScalarType type_;
  bool bool_;
  common::ObString text_;   // decoded string or the raw number literal
};

// Newline delimited JSON reader that accepts one flat object per line.
// Nested objects / arrays and non-object documents are rejected with the line
// number. Blank lines are skipped.
class JsonlReader
{
public:
  JsonlReader() : input_(), max_line_size_(FILE_SQL_DEFAULT_MAX_LINE_SIZE), line_(), decoded_(),
                  members_(), line_no_(0), object_line_(0), recoverable_(true)
  { error_msg_[0] = '\0'; }
  int open(const common::ObString &path, const FileFingerprint *expect, const int64_t max_line_size);
  int rewind();
  void close() { input_.close(); }
  int next_object(bool &has_object);
  const common::ObIArray<JsonMember> &get_members() const { return members_; }
  int64_t get_object_line() const { return object_line_; }
  const char *get_error_msg() const { return error_msg_; }
  bool is_recoverable() const { return recoverable_; }
  const FileFingerprint &get_fingerprint() const { return input_.get_fingerprint(); }
  // Exposed for unit tests: parses one line into members.
  int parse_line(const char *line, const int64_t len);
private:
  int read_line(bool &has_line);
  int set_error(const char *fmt, ...);
  int parse_string(const char *s, const int64_t len, int64_t &pos, common::ObString &out);
  int parse_number(const char *s, const int64_t len, int64_t &pos, common::ObString &out);
  DISALLOW_COPY_AND_ASSIGN(JsonlReader);
  BufferedFileInput input_;
  int64_t max_line_size_;
  FileBuffer line_;
  FileBuffer decoded_;
  common::ObSEArray<JsonMember, 32> members_;
  int64_t line_no_;
  int64_t object_line_;
  bool recoverable_;
  char error_msg_[FILE_SQL_ERROR_MSG_LEN];
};

}  // namespace file_sql
}  // namespace sql
}  // namespace oceanbase
