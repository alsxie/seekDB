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

#define USING_LOG_PREFIX SQL_ENG

#include "sql/engine/basic/file_sql/file_text_reader.h"

#include <cstdarg>
#include <cstdio>
#include <unistd.h>

#include "lib/oblog/ob_log_module.h"

namespace oceanbase
{
using namespace common;
namespace sql
{
namespace file_sql
{

// --------------------------------------------------------- BufferedFileInput

int BufferedFileInput::open(const ObString &path, const FileFingerprint *expect)
{
  int ret = OB_SUCCESS;
  close();
  if (path.length() > PATH_MAX) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("path too long", K(ret));
  } else {
    MEMCPY(path_buf_, path.ptr(), path.length());
    path_buf_[path.length()] = '\0';
    path_.assign_ptr(path_buf_, path.length());
    if (OB_FAIL(open_input_file(path_, expect, fd_, fingerprint_))) {
      LOG_WARN("failed to open file", K(ret), K(path_));
    } else if (OB_FAIL(buf_.reserve(FILE_SQL_READ_BLOCK_SIZE))) {
      LOG_WARN("failed to reserve buffer", K(ret));
    } else if (OB_FAIL(skip_bom())) {
      LOG_WARN("failed to skip bom", K(ret));
    }
  }
  return ret;
}

int BufferedFileInput::rewind()
{
  int ret = OB_SUCCESS;
  if (fd_ < 0) {
    ret = OB_NOT_INIT;
    LOG_WARN("file is not open", K(ret));
  } else if (OB_FAIL(check_file_unchanged(fd_, path_, fingerprint_))) {
    LOG_WARN("file changed", K(ret));
  } else {
    pos_ = 0;
    len_ = 0;
    offset_ = 0;
    reached_eof_ = false;
    if (OB_FAIL(skip_bom())) {
      LOG_WARN("failed to skip bom", K(ret));
    }
  }
  return ret;
}

void BufferedFileInput::close()
{
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
  buf_.reset();
  pos_ = 0;
  len_ = 0;
  offset_ = 0;
  reached_eof_ = false;
}

int BufferedFileInput::skip_bom()
{
  int ret = OB_SUCCESS;
  char c = 0;
  bool eof = false;
  // load the first block and drop a UTF-8 BOM
  if (OB_FAIL(fill(c, eof))) {
  } else if (!eof) {
    --pos_;  // un-read the byte returned by fill()
    if (len_ >= 3 && 0 == MEMCMP(buf_.ptr(), "\xEF\xBB\xBF", 3)) {
      pos_ = 3;
    }
  }
  return ret;
}

int BufferedFileInput::fill(char &c, bool &eof)
{
  int ret = OB_SUCCESS;
  eof = false;
  if (reached_eof_) {
    eof = true;
  } else {
    int64_t read_len = 0;
    if (OB_FAIL(pread_full(fd_, buf_.ptr(), buf_.capacity(), offset_, read_len))) {
      LOG_WARN("failed to read file", K(ret), K(path_));
    } else {
      offset_ += read_len;
      pos_ = 0;
      len_ = read_len;
      if (0 == read_len || offset_ > fingerprint_.size_) {
        // EOF, or the file grew while being read
        reached_eof_ = (0 == read_len);
        if (OB_FAIL(check_file_unchanged(fd_, path_, fingerprint_))) {
          LOG_WARN("file changed during scan", K(ret));
        } else if (offset_ > fingerprint_.size_) {
          ret = OB_INVALID_DATA;
          FORWARD_USER_ERROR_MSG(ret, "file '%.*s' has changed during the scan",
                                 path_.length(), path_.ptr());
        }
      }
      if (OB_SUCC(ret)) {
        if (reached_eof_) {
          eof = true;
        } else {
          c = buf_.ptr()[pos_++];
        }
      }
    }
  }
  return ret;
}

// ----------------------------------------------------------- CsvRecordReader

int CsvRecordReader::open(const ObString &path, const FileFingerprint *expect,
                          const CsvOptions &opt, const int64_t max_line_size)
{
  int ret = OB_SUCCESS;
  opt_ = opt;
  max_line_size_ = max_line_size;
  line_no_ = 1;
  recoverable_ = true;
  if (OB_FAIL(input_.open(path, expect))) {
    LOG_WARN("failed to open csv", K(ret));
  }
  return ret;
}

int CsvRecordReader::rewind()
{
  line_no_ = 1;
  recoverable_ = true;
  return input_.rewind();
}

int CsvRecordReader::set_error(const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(error_msg_, sizeof(error_msg_), fmt, ap);
  va_end(ap);
  return OB_INVALID_DATA;
}

int CsvRecordReader::end_field(const int64_t start, const bool quoted, const bool strip_cr)
{
  int ret = OB_SUCCESS;
  int64_t len = record_.length() - start;
  if (strip_cr && len > 0 && '\r' == record_.ptr()[record_.length() - 1]) {
    --len;
  }
  if (fields_.count() >= FILE_SQL_MAX_COLUMNS) {
    ret = set_error("too many columns (limit %ld)", FILE_SQL_MAX_COLUMNS);
  } else if (OB_FAIL(fields_.push_back(FieldPos(start, len, quoted)))) {
    LOG_WARN("push back failed", K(ret));
  }
  return ret;
}

int CsvRecordReader::skip_to_line_end()
{
  int ret = OB_SUCCESS;
  bool eof = false;
  char c = 0;
  while (OB_SUCC(ret) && !eof) {
    if (OB_FAIL(input_.next(c, eof))) {
    } else if (!eof && '\n' == c) {
      ++line_no_;
      break;
    }
  }
  return ret;
}

// States of the record state machine.
enum CsvState
{
  CSV_FIELD_START = 0,
  CSV_UNQUOTED = 1,
  CSV_QUOTED = 2,
  CSV_QUOTE_IN_QUOTED = 3,  // saw quote inside quoted field (escape == quote)
  CSV_AFTER_QUOTED = 4,     // closing quote consumed, expecting delim / newline
};

int CsvRecordReader::next_record(bool &has_record)
{
  int ret = OB_SUCCESS;
  has_record = false;
  bool done = false;
  error_msg_[0] = '\0';
  while (OB_SUCC(ret) && !done) {
    record_.reuse();
    fields_.reuse();
    record_line_ = line_no_;
    CsvState state = CSV_FIELD_START;
    int64_t field_start = 0;
    bool quoted = false;
    bool any_byte = false;
    bool eof = false;
    bool record_end = false;
    char c = 0;
    while (OB_SUCC(ret) && !record_end) {
      if (OB_FAIL(input_.next(c, eof))) {
        LOG_WARN("failed to read", K(ret));
      } else if (eof) {
        if (CSV_QUOTED == state) {
          recoverable_ = false;
          ret = set_error("line %ld: unterminated quoted field", record_line_);
        } else if (any_byte) {
          ret = end_field(field_start, quoted, !quoted);
        }
        record_end = true;
      } else if (record_.length() >= max_line_size_) {
        ret = set_error("line %ld: record exceeds max_line_size (%ld bytes)",
                        record_line_, max_line_size_);
        if (CSV_QUOTED != state && '\n' != c) {
          int tmp_ret = skip_to_line_end();
          ret = OB_SUCCESS == tmp_ret ? ret : tmp_ret;
        } else if (CSV_QUOTED == state) {
          recoverable_ = false;
        }
        record_end = true;
      } else {
        any_byte = true;
        switch (state) {
          case CSV_FIELD_START: {
            quoted = false;
            field_start = record_.length();
            if ('\0' != opt_.quote_ && c == opt_.quote_) {
              quoted = true;
              state = CSV_QUOTED;
            } else if (c == opt_.delim_) {
              ret = end_field(field_start, false, false);
            } else if ('\n' == c) {
              ++line_no_;
              ret = end_field(field_start, false, true);
              record_end = true;
            } else {
              ret = record_.push_back(c);
              state = CSV_UNQUOTED;
            }
            break;
          }
          case CSV_UNQUOTED: {
            if (c == opt_.delim_) {
              ret = end_field(field_start, false, false);
              state = CSV_FIELD_START;
            } else if ('\n' == c) {
              ++line_no_;
              ret = end_field(field_start, false, true);
              record_end = true;
            } else {
              // a quote inside an unquoted field is kept literally
              ret = record_.push_back(c);
            }
            break;
          }
          case CSV_QUOTED: {
            if (opt_.escape_ == opt_.quote_ && c == opt_.quote_) {
              state = CSV_QUOTE_IN_QUOTED;
            } else if (c == opt_.escape_) {
              char next = 0;
              bool next_eof = false;
              if (OB_FAIL(input_.next(next, next_eof))) {
              } else if (next_eof) {
                recoverable_ = false;
                ret = set_error("line %ld: unterminated quoted field", record_line_);
                record_end = true;
              } else {
                if ('\n' == next) {
                  ++line_no_;
                }
                ret = record_.push_back(next);
              }
            } else if (c == opt_.quote_) {
              state = CSV_AFTER_QUOTED;
            } else {
              if ('\n' == c) {
                ++line_no_;
              }
              ret = record_.push_back(c);
            }
            break;
          }
          case CSV_QUOTE_IN_QUOTED: {
            if (c == opt_.quote_) {
              ret = record_.push_back(c);
              state = CSV_QUOTED;
            } else if (c == opt_.delim_) {
              ret = end_field(field_start, true, false);
              state = CSV_FIELD_START;
            } else if ('\n' == c) {
              ++line_no_;
              ret = end_field(field_start, true, false);
              record_end = true;
            } else if ('\r' == c) {
              state = CSV_AFTER_QUOTED;
            } else {
              ret = set_error("line %ld: unexpected character after closing quote", record_line_);
              int tmp_ret = skip_to_line_end();
              ret = OB_SUCCESS == tmp_ret ? ret : tmp_ret;
              record_end = true;
            }
            break;
          }
          case CSV_AFTER_QUOTED: {
            if (c == opt_.delim_) {
              ret = end_field(field_start, true, false);
              state = CSV_FIELD_START;
            } else if ('\n' == c) {
              ++line_no_;
              ret = end_field(field_start, true, false);
              record_end = true;
            } else if ('\r' == c) {
              // CRLF after a quoted field
            } else {
              ret = set_error("line %ld: unexpected character after closing quote", record_line_);
              int tmp_ret = skip_to_line_end();
              ret = OB_SUCCESS == tmp_ret ? ret : tmp_ret;
              record_end = true;
            }
            break;
          }
          default: {
            ret = OB_ERR_UNEXPECTED;
          }
        }
      }
    }
    if (OB_SUCC(ret)) {
      if (eof && fields_.empty()) {
        done = true;  // end of file
      } else if (1 == fields_.count() && !fields_.at(0).quoted_ && 0 == fields_.at(0).len_) {
        // blank line, skip it
        if (eof) {
          done = true;
        }
      } else {
        has_record = true;
        done = true;
      }
    } else {
      done = true;
    }
  }
  if (OB_SUCC(ret)) {
    // quoted bytes are validated here, once per record
    if (has_record && !TextValueParser::is_valid_utf8(record_.ptr(), record_.length())) {
      ret = set_error("line %ld: invalid UTF-8 sequence", record_line_);
      has_record = false;
    }
  }
  return ret;
}

// --------------------------------------------------------------- JsonlReader

int JsonlReader::open(const ObString &path, const FileFingerprint *expect, const int64_t max_line_size)
{
  int ret = OB_SUCCESS;
  max_line_size_ = max_line_size;
  line_no_ = 0;
  recoverable_ = true;
  if (OB_FAIL(input_.open(path, expect))) {
    LOG_WARN("failed to open jsonl", K(ret));
  }
  return ret;
}

int JsonlReader::rewind()
{
  line_no_ = 0;
  recoverable_ = true;
  return input_.rewind();
}

int JsonlReader::set_error(const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(error_msg_, sizeof(error_msg_), fmt, ap);
  va_end(ap);
  return OB_INVALID_DATA;
}

int JsonlReader::read_line(bool &has_line)
{
  int ret = OB_SUCCESS;
  bool eof = false;
  bool any = false;
  bool too_long = false;
  char c = 0;
  has_line = false;
  line_.reuse();
  while (OB_SUCC(ret)) {
    if (OB_FAIL(input_.next(c, eof))) {
      LOG_WARN("read failed", K(ret));
    } else if (eof) {
      break;
    } else {
      any = true;
      if ('\n' == c) {
        break;
      } else if (line_.length() >= max_line_size_) {
        too_long = true;
      } else if (OB_FAIL(line_.push_back(c))) {
      }
    }
  }
  if (OB_SUCC(ret) && any) {
    ++line_no_;
    has_line = true;
    if (too_long) {
      ret = set_error("line %ld: record exceeds max_line_size (%ld bytes)", line_no_, max_line_size_);
    }
  }
  return ret;
}

static inline bool is_json_ws(const char c)
{
  return ' ' == c || '\t' == c || '\r' == c || '\n' == c;
}

static inline void skip_ws(const char *s, const int64_t len, int64_t &pos)
{
  while (pos < len && is_json_ws(s[pos])) {
    ++pos;
  }
}

static int hex_value(const char c)
{
  int v = -1;
  if (c >= '0' && c <= '9') { v = c - '0'; }
  else if (c >= 'a' && c <= 'f') { v = c - 'a' + 10; }
  else if (c >= 'A' && c <= 'F') { v = c - 'A' + 10; }
  return v;
}

static int append_utf8(FileBuffer &buf, uint32_t cp)
{
  int ret = OB_SUCCESS;
  char tmp[4];
  int64_t n = 0;
  if (cp < 0x80) {
    tmp[n++] = static_cast<char>(cp);
  } else if (cp < 0x800) {
    tmp[n++] = static_cast<char>(0xC0 | (cp >> 6));
    tmp[n++] = static_cast<char>(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    tmp[n++] = static_cast<char>(0xE0 | (cp >> 12));
    tmp[n++] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    tmp[n++] = static_cast<char>(0x80 | (cp & 0x3F));
  } else {
    tmp[n++] = static_cast<char>(0xF0 | (cp >> 18));
    tmp[n++] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    tmp[n++] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    tmp[n++] = static_cast<char>(0x80 | (cp & 0x3F));
  }
  // capacity is reserved up front so this never reallocates (see parse_line)
  if (buf.length() + n > buf.capacity()) {
    ret = OB_SIZE_OVERFLOW;
  } else {
    ret = buf.append(tmp, n);
  }
  return ret;
}

int JsonlReader::parse_string(const char *s, const int64_t len, int64_t &pos, ObString &out)
{
  int ret = OB_SUCCESS;
  // s[pos] == '"'
  ++pos;
  const int64_t start = decoded_.length();
  bool closed = false;
  while (OB_SUCC(ret) && pos < len && !closed) {
    const unsigned char c = static_cast<unsigned char>(s[pos]);
    if ('"' == c) {
      closed = true;
      ++pos;
    } else if ('\\' == c) {
      if (pos + 1 >= len) {
        ret = set_error("line %ld: invalid escape in string", line_no_);
      } else {
        const char e = s[pos + 1];
        pos += 2;
        char out_c = 0;
        switch (e) {
          case '"': out_c = '"'; break;
          case '\\': out_c = '\\'; break;
          case '/': out_c = '/'; break;
          case 'b': out_c = '\b'; break;
          case 'f': out_c = '\f'; break;
          case 'n': out_c = '\n'; break;
          case 'r': out_c = '\r'; break;
          case 't': out_c = '\t'; break;
          case 'u': {
            uint32_t cp = 0;
            for (int64_t k = 0; OB_SUCC(ret) && k < 4; ++k) {
              const int h = pos + k < len ? hex_value(s[pos + k]) : -1;
              if (h < 0) {
                ret = set_error("line %ld: invalid \\u escape", line_no_);
              } else {
                cp = (cp << 4) | static_cast<uint32_t>(h);
              }
            }
            if (OB_SUCC(ret)) {
              pos += 4;
              if (cp >= 0xD800 && cp <= 0xDBFF) {
                uint32_t lo = 0;
                bool ok = pos + 6 <= len && '\\' == s[pos] && 'u' == s[pos + 1];
                for (int64_t k = 0; ok && k < 4; ++k) {
                  const int h = hex_value(s[pos + 2 + k]);
                  ok = h >= 0;
                  lo = (lo << 4) | static_cast<uint32_t>(h);
                }
                if (!ok || lo < 0xDC00 || lo > 0xDFFF) {
                  ret = set_error("line %ld: invalid surrogate pair", line_no_);
                } else {
                  pos += 6;
                  cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                }
              } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                ret = set_error("line %ld: invalid surrogate pair", line_no_);
              }
            }
            if (OB_SUCC(ret) && OB_FAIL(append_utf8(decoded_, cp))) {
              LOG_WARN("append failed", K(ret));
            }
            break;
          }
          default: {
            ret = set_error("line %ld: invalid escape in string", line_no_);
          }
        }
        if (OB_SUCC(ret) && 'u' != e) {
          ret = decoded_.push_back(out_c);
        }
      }
    } else if (c < 0x20) {
      ret = set_error("line %ld: control character in string", line_no_);
    } else {
      ret = decoded_.push_back(static_cast<char>(c));
      ++pos;
    }
  }
  if (OB_SUCC(ret) && !closed) {
    ret = set_error("line %ld: unterminated string", line_no_);
  }
  if (OB_SUCC(ret)) {
    out.assign_ptr(decoded_.ptr() + start, static_cast<int32_t>(decoded_.length() - start));
  }
  return ret;
}

int JsonlReader::parse_number(const char *s, const int64_t len, int64_t &pos, ObString &out)
{
  int ret = OB_SUCCESS;
  const int64_t start = pos;
  if (pos < len && '-' == s[pos]) {
    ++pos;
  }
  if (pos < len && '0' == s[pos]) {
    ++pos;
  } else if (pos < len && s[pos] >= '1' && s[pos] <= '9') {
    while (pos < len && s[pos] >= '0' && s[pos] <= '9') { ++pos; }
  } else {
    ret = set_error("line %ld: invalid number", line_no_);
  }
  if (OB_SUCC(ret) && pos < len && '.' == s[pos]) {
    ++pos;
    const int64_t d = pos;
    while (pos < len && s[pos] >= '0' && s[pos] <= '9') { ++pos; }
    if (d == pos) {
      ret = set_error("line %ld: invalid number", line_no_);
    }
  }
  if (OB_SUCC(ret) && pos < len && ('e' == s[pos] || 'E' == s[pos])) {
    ++pos;
    if (pos < len && ('+' == s[pos] || '-' == s[pos])) { ++pos; }
    const int64_t d = pos;
    while (pos < len && s[pos] >= '0' && s[pos] <= '9') { ++pos; }
    if (d == pos) {
      ret = set_error("line %ld: invalid number", line_no_);
    }
  }
  if (OB_SUCC(ret)) {
    out.assign_ptr(s + start, static_cast<int32_t>(pos - start));
  }
  return ret;
}

int JsonlReader::parse_line(const char *s, const int64_t len)
{
  int ret = OB_SUCCESS;
  int64_t pos = 0;
  members_.reuse();
  decoded_.reuse();
  // decoded strings never exceed the raw text; reserving keeps pointers stable
  if (OB_FAIL(decoded_.reserve(len + 8))) {
    LOG_WARN("reserve failed", K(ret));
  } else if (!TextValueParser::is_valid_utf8(s, len)) {
    ret = set_error("line %ld: invalid UTF-8 sequence", line_no_);
  }
  skip_ws(s, len, pos);
  if (OB_FAIL(ret)) {
  } else if (pos >= len || '{' != s[pos]) {
    ret = set_error("line %ld: expected a JSON object", line_no_);
  } else {
    ++pos;
    skip_ws(s, len, pos);
    bool end = false;
    if (pos < len && '}' == s[pos]) {
      ++pos;
      end = true;
    }
    while (OB_SUCC(ret) && !end) {
      JsonMember m;
      skip_ws(s, len, pos);
      if (pos >= len || '"' != s[pos]) {
        ret = set_error("line %ld: expected an object key", line_no_);
      } else if (OB_FAIL(parse_string(s, len, pos, m.key_))) {
      } else {
        skip_ws(s, len, pos);
        if (pos >= len || ':' != s[pos]) {
          ret = set_error("line %ld: expected ':' after object key", line_no_);
        } else {
          ++pos;
          skip_ws(s, len, pos);
        }
      }
      if (OB_FAIL(ret)) {
      } else if (pos >= len) {
        ret = set_error("line %ld: unexpected end of line", line_no_);
      } else if ('{' == s[pos] || '[' == s[pos]) {
        ret = set_error("line %ld: nested value of key '%.*s' is not supported",
                        line_no_, m.key_.length(), m.key_.ptr());
      } else if ('"' == s[pos]) {
        m.type_ = JsonScalarType::JSON_STRING;
        ret = parse_string(s, len, pos, m.text_);
      } else if (pos + 4 <= len && 0 == MEMCMP(s + pos, "true", 4)) {
        m.type_ = JsonScalarType::JSON_BOOL;
        m.bool_ = true;
        pos += 4;
      } else if (pos + 5 <= len && 0 == MEMCMP(s + pos, "false", 5)) {
        m.type_ = JsonScalarType::JSON_BOOL;
        m.bool_ = false;
        pos += 5;
      } else if (pos + 4 <= len && 0 == MEMCMP(s + pos, "null", 4)) {
        m.type_ = JsonScalarType::JSON_NULL;
        pos += 4;
      } else {
        m.type_ = JsonScalarType::JSON_NUMBER;
        ret = parse_number(s, len, pos, m.text_);
      }
      if (OB_SUCC(ret)) {
        // duplicate keys: the last one wins, as in most JSON readers
        bool replaced = false;
        for (int64_t i = 0; !replaced && i < members_.count(); ++i) {
          if (members_.at(i).key_ == m.key_) {
            members_.at(i) = m;
            replaced = true;
          }
        }
        if (replaced) {
        } else if (members_.count() >= FILE_SQL_MAX_COLUMNS) {
          ret = set_error("line %ld: too many keys (limit %ld)", line_no_, FILE_SQL_MAX_COLUMNS);
        } else if (OB_FAIL(members_.push_back(m))) {
          LOG_WARN("push back failed", K(ret));
        }
      }
      if (OB_SUCC(ret)) {
        skip_ws(s, len, pos);
        if (pos < len && ',' == s[pos]) {
          ++pos;
        } else if (pos < len && '}' == s[pos]) {
          ++pos;
          end = true;
        } else {
          ret = set_error("line %ld: expected ',' or '}'", line_no_);
        }
      }
    }
    if (OB_SUCC(ret)) {
      skip_ws(s, len, pos);
      if (pos != len) {
        ret = set_error("line %ld: unexpected trailing characters", line_no_);
      }
    }
  }
  return ret;
}

int JsonlReader::next_object(bool &has_object)
{
  int ret = OB_SUCCESS;
  has_object = false;
  error_msg_[0] = '\0';
  bool done = false;
  while (OB_SUCC(ret) && !done) {
    bool has_line = false;
    if (OB_FAIL(read_line(has_line))) {
    } else if (!has_line) {
      done = true;
    } else {
      int64_t pos = 0;
      skip_ws(line_.ptr(), line_.length(), pos);
      if (pos == line_.length()) {
        // blank line
      } else if (OB_FAIL(parse_line(line_.ptr(), line_.length()))) {
      } else {
        object_line_ = line_no_;
        has_object = true;
        done = true;
      }
    }
  }
  if (OB_FAIL(ret)) {
    object_line_ = line_no_;
  }
  return ret;
}

}  // namespace file_sql
}  // namespace sql
}  // namespace oceanbase
