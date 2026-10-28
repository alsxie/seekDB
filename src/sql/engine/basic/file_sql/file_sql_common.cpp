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

#include "sql/engine/basic/file_sql/file_sql_common.h"

#include <cerrno>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "lib/alloc/alloc_struct.h"
#include "lib/allocator/ob_malloc.h"
#include "lib/oblog/ob_log_module.h"
#include "lib/utility/serialization.h"
#include "share/ob_errno.h"

namespace oceanbase
{
using namespace common;
namespace sql
{
namespace file_sql
{

const char *get_file_format_name(FileFormat format)
{
  const char *name = "unknown";
  switch (format) {
    case FileFormat::CSV: name = "csv"; break;
    case FileFormat::JSONL: name = "json"; break;
    case FileFormat::PARQUET: name = "parquet"; break;
    case FileFormat::GLOB: name = "glob"; break;
    default: break;
  }
  return name;
}

const char *get_file_column_type_name(FileColumnType type)
{
  const char *name = "VARCHAR";
  switch (type) {
    case FileColumnType::NULL_TYPE: name = "NULL"; break;
    case FileColumnType::BOOLEAN: name = "BOOLEAN"; break;
    case FileColumnType::BIGINT: name = "BIGINT"; break;
    case FileColumnType::DOUBLE: name = "DOUBLE"; break;
    case FileColumnType::DATE: name = "DATE"; break;
    case FileColumnType::DATETIME: name = "DATETIME"; break;
    case FileColumnType::VARCHAR: name = "VARCHAR"; break;
    default: break;
  }
  return name;
}

// ---------------------------------------------------------------- FileBuffer

int FileBuffer::reserve(const int64_t cap)
{
  int ret = OB_SUCCESS;
  if (cap > cap_) {
    int64_t new_cap = MAX(cap, MAX(cap_ * 2, 4096L));
    char *new_buf = static_cast<char *>(ob_malloc(new_cap, ObMemAttr("FileSqlBuf")));
    if (OB_ISNULL(new_buf)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
      LOG_WARN("failed to allocate file sql buffer", K(ret), K(new_cap));
    } else {
      if (len_ > 0) {
        MEMCPY(new_buf, buf_, len_);
      }
      if (OB_NOT_NULL(buf_)) {
        ob_free(buf_);
      }
      buf_ = new_buf;
      cap_ = new_cap;
    }
  }
  return ret;
}

int FileBuffer::append(const char *data, const int64_t len)
{
  int ret = OB_SUCCESS;
  if (len <= 0) {
  } else if (OB_FAIL(reserve(len_ + len))) {
  } else {
    MEMCPY(buf_ + len_, data, len);
    len_ += len;
  }
  return ret;
}

int FileBuffer::resize(const int64_t len)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(reserve(len))) {
  } else {
    len_ = len;
  }
  return ret;
}

void FileBuffer::reset()
{
  if (OB_NOT_NULL(buf_)) {
    ob_free(buf_);
  }
  buf_ = nullptr;
  len_ = 0;
  cap_ = 0;
}

// ----------------------------------------------------------- FileFingerprint

int FileFingerprint::load_from_fd(const int fd)
{
  int ret = OB_SUCCESS;
  struct stat st;
  if (0 != ::fstat(fd, &st)) {
    ret = OB_IO_ERROR;
    LOG_WARN("fstat failed", K(ret), K(errno));
  } else {
    dev_ = static_cast<int64_t>(st.st_dev);
    ino_ = static_cast<int64_t>(st.st_ino);
    size_ = static_cast<int64_t>(st.st_size);
#ifdef __APPLE__
    mtime_ns_ = static_cast<int64_t>(st.st_mtimespec.tv_sec) * 1000000000LL + st.st_mtimespec.tv_nsec;
#else
    mtime_ns_ = static_cast<int64_t>(st.st_mtim.tv_sec) * 1000000000LL + st.st_mtim.tv_nsec;
#endif
  }
  return ret;
}

// ----------------------------------------------------------- TextValueParser

static inline bool is_digit(const char c) { return c >= '0' && c <= '9'; }

bool TextValueParser::parse_bool(const char *str, const int64_t len, bool &value)
{
  bool ok = false;
  if (4 == len && 0 == strncasecmp(str, "true", 4)) {
    value = true;
    ok = true;
  } else if (5 == len && 0 == strncasecmp(str, "false", 5)) {
    value = false;
    ok = true;
  }
  return ok;
}

bool TextValueParser::parse_bigint(const char *str, const int64_t len, int64_t &value,
                                   bool &out_of_range)
{
  bool ok = false;
  out_of_range = false;
  int64_t pos = 0;
  bool neg = false;
  if (len > 0 && ('+' == str[0] || '-' == str[0])) {
    neg = ('-' == str[0]);
    pos = 1;
  }
  if (pos < len) {
    ok = true;
    uint64_t acc = 0;
    const uint64_t limit = neg ? static_cast<uint64_t>(INT64_MAX) + 1 : static_cast<uint64_t>(INT64_MAX);
    for (; ok && pos < len; ++pos) {
      if (!is_digit(str[pos])) {
        ok = false;
      } else if (!out_of_range) {
        const uint64_t d = static_cast<uint64_t>(str[pos] - '0');
        if (acc > (limit - d) / 10) {
          out_of_range = true;
        } else {
          acc = acc * 10 + d;
        }
      }
    }
    if (ok && !out_of_range) {
      value = neg ? static_cast<int64_t>(0 - acc) : static_cast<int64_t>(acc);
    }
  }
  return ok;
}

bool TextValueParser::parse_double(const char *str, const int64_t len, double &value)
{
  bool ok = false;
  int64_t pos = 0;
  if (len > 0 && len < 256) {
    if ('+' == str[pos] || '-' == str[pos]) {
      ++pos;
    }
    int64_t int_digits = 0;
    int64_t frac_digits = 0;
    while (pos < len && is_digit(str[pos])) { ++pos; ++int_digits; }
    if (pos < len && '.' == str[pos]) {
      ++pos;
      while (pos < len && is_digit(str[pos])) { ++pos; ++frac_digits; }
    }
    ok = (int_digits + frac_digits > 0);
    if (ok && pos < len && ('e' == str[pos] || 'E' == str[pos])) {
      ++pos;
      if (pos < len && ('+' == str[pos] || '-' == str[pos])) {
        ++pos;
      }
      int64_t exp_digits = 0;
      while (pos < len && is_digit(str[pos])) { ++pos; ++exp_digits; }
      ok = exp_digits > 0;
    }
    ok = ok && pos == len;
    if (ok) {
      char buf[256];
      MEMCPY(buf, str, len);
      buf[len] = '\0';
      char *end = nullptr;
      errno = 0;
      const double v = strtod(buf, &end);
      ok = (end == buf + len) && std::isfinite(v);
      if (ok) {
        value = v;
      }
    }
  }
  return ok;
}

bool TextValueParser::is_valid_date(int64_t year, int64_t month, int64_t day)
{
  static const int8_t DAYS[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  bool ok = year >= 1 && year <= 9999 && month >= 1 && month <= 12 && day >= 1;
  if (ok) {
    int64_t max_day = DAYS[month - 1];
    if (2 == month && ((0 == year % 4 && 0 != year % 100) || 0 == year % 400)) {
      max_day = 29;
    }
    ok = day <= max_day;
  }
  return ok;
}

// Howard Hinnant's days_from_civil, days relative to 1970-01-01.
int64_t TextValueParser::days_from_civil(int64_t y, int64_t m, int64_t d)
{
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const int64_t yoe = y - era * 400;
  const int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

static bool parse_fixed_digits(const char *str, const int64_t n, int64_t &v)
{
  bool ok = true;
  v = 0;
  for (int64_t i = 0; ok && i < n; ++i) {
    ok = is_digit(str[i]);
    v = v * 10 + (str[i] - '0');
  }
  return ok;
}

static bool parse_date_part(const char *str, const int64_t len, int64_t &days)
{
  int64_t y = 0;
  int64_t m = 0;
  int64_t d = 0;
  bool ok = len == 10 && '-' == str[4] && '-' == str[7]
            && parse_fixed_digits(str, 4, y)
            && parse_fixed_digits(str + 5, 2, m)
            && parse_fixed_digits(str + 8, 2, d)
            && TextValueParser::is_valid_date(y, m, d);
  if (ok) {
    days = TextValueParser::days_from_civil(y, m, d);
  }
  return ok;
}

bool TextValueParser::parse_date(const char *str, const int64_t len, int32_t &days)
{
  int64_t d = 0;
  bool ok = parse_date_part(str, len, d);
  if (ok) {
    days = static_cast<int32_t>(d);
  }
  return ok;
}

bool TextValueParser::parse_datetime(const char *str, const int64_t len, int64_t &usec, int16_t &scale)
{
  bool ok = false;
  int64_t days = 0;
  int64_t h = 0;
  int64_t mi = 0;
  int64_t s = 0;
  if (len >= 19 && parse_date_part(str, 10, days) && (' ' == str[10] || 'T' == str[10])
      && ':' == str[13] && ':' == str[16]
      && parse_fixed_digits(str + 11, 2, h) && parse_fixed_digits(str + 14, 2, mi)
      && parse_fixed_digits(str + 17, 2, s) && h <= 23 && mi <= 59 && s <= 59) {
    int64_t frac = 0;
    int64_t digits = 0;
    ok = true;
    if (len > 19) {
      ok = '.' == str[19] && len > 20 && len <= 26;
      for (int64_t i = 20; ok && i < len; ++i) {
        ok = is_digit(str[i]);
        frac = frac * 10 + (str[i] - '0');
        ++digits;
      }
    }
    if (ok) {
      for (int64_t i = digits; i < 6; ++i) {
        frac *= 10;
      }
      scale = static_cast<int16_t>(digits);
      usec = days * USECS_PER_DAY + ((h * 60 + mi) * 60 + s) * 1000000LL + frac;
    }
  }
  return ok;
}

bool TextValueParser::parse_date_or_datetime(const char *str, const int64_t len, int64_t &usec)
{
  int64_t days = 0;
  int16_t scale = 0;
  bool ok = false;
  if (parse_date_part(str, len, days)) {
    usec = days * USECS_PER_DAY;
    ok = true;
  } else {
    ok = parse_datetime(str, len, usec, scale);
  }
  return ok;
}

bool TextValueParser::is_valid_utf8(const char *str, const int64_t len)
{
  const unsigned char *s = reinterpret_cast<const unsigned char *>(str);
  int64_t i = 0;
  bool ok = true;
  while (ok && i < len) {
    const unsigned char c = s[i];
    if (c < 0x80) {
      ++i;
    } else {
      int64_t n = 0;
      uint32_t cp = 0;
      uint32_t min_cp = 0;
      if (c >= 0xC2 && c <= 0xDF) { n = 1; cp = c & 0x1F; min_cp = 0x80; }
      else if (c >= 0xE0 && c <= 0xEF) { n = 2; cp = c & 0x0F; min_cp = 0x800; }
      else if (c >= 0xF0 && c <= 0xF4) { n = 3; cp = c & 0x07; min_cp = 0x10000; }
      else { ok = false; }
      if (ok && i + n >= len) {
        ok = false;  // truncated sequence
      }
      for (int64_t k = 1; ok && k <= n; ++k) {
        if ((s[i + k] & 0xC0) != 0x80) {
          ok = false;
        } else {
          cp = (cp << 6) | (s[i + k] & 0x3F);
        }
      }
      if (ok) {
        ok = cp >= min_cp && cp <= 0x10FFFF && !(cp >= 0xD800 && cp <= 0xDFFF);
        i += n + 1;
      }
    }
  }
  return ok;
}

int64_t TextValueParser::utf8_char_length(const char *str, const int64_t len)
{
  int64_t n = 0;
  for (int64_t i = 0; i < len; ++i) {
    if ((static_cast<unsigned char>(str[i]) & 0xC0) != 0x80) {
      ++n;
    }
  }
  return n;
}

// ---------------------------------------------------------- ColumnInferState

void ColumnInferState::add_mask(const uint8_t mask, const char *str, const int64_t len)
{
  has_value_ = true;
  mask_ &= mask;
  max_char_length_ = MAX(max_char_length_, TextValueParser::utf8_char_length(str, len));
}

void ColumnInferState::add_text(const char *str, const int64_t len)
{
  uint8_t m = MASK_VARCHAR;
  bool b = false;
  int64_t i = 0;
  double d = 0;
  int32_t days = 0;
  int64_t usec = 0;
  int16_t scale = 0;
  bool out_of_range = false;
  if (len > 0) {
    if (TextValueParser::parse_bool(str, len, b)) {
      m |= MASK_BOOLEAN;
    } else if (TextValueParser::parse_bigint(str, len, i, out_of_range)) {
      m |= out_of_range ? 0 : MASK_BIGINT;
      if (TextValueParser::parse_double(str, len, d)) {
        m |= MASK_DOUBLE;
      }
    } else if (TextValueParser::parse_double(str, len, d)) {
      m |= MASK_DOUBLE;
    } else if (TextValueParser::parse_date(str, len, days)) {
      m |= MASK_DATE | MASK_DATETIME;
    } else if (TextValueParser::parse_datetime(str, len, usec, scale)) {
      m |= MASK_DATETIME;
      datetime_scale_ = MAX(datetime_scale_, scale);
    }
  }
  add_mask(m, str, len);
}

void ColumnInferState::add_json_bool()
{
  add_mask(MASK_BOOLEAN | MASK_VARCHAR, "false", 5);
}

void ColumnInferState::add_json_number(const char *str, const int64_t len)
{
  uint8_t m = MASK_VARCHAR;
  int64_t i = 0;
  double d = 0;
  bool out_of_range = false;
  if (TextValueParser::parse_double(str, len, d)) {
    m |= MASK_DOUBLE;
  }
  if (TextValueParser::parse_bigint(str, len, i, out_of_range) && !out_of_range) {
    m |= MASK_BIGINT;
  }
  add_mask(m, str, len);
}

void ColumnInferState::add_json_string(const char *str, const int64_t len)
{
  uint8_t m = MASK_VARCHAR;
  int32_t days = 0;
  int64_t usec = 0;
  int16_t scale = 0;
  if (TextValueParser::parse_date(str, len, days)) {
    m |= MASK_DATE | MASK_DATETIME;
  } else if (TextValueParser::parse_datetime(str, len, usec, scale)) {
    m |= MASK_DATETIME;
    datetime_scale_ = MAX(datetime_scale_, scale);
  }
  add_mask(m, str, len);
}

FileColumnType ColumnInferState::get_type() const
{
  FileColumnType type = FileColumnType::VARCHAR;
  if (!has_value_) {
    type = FileColumnType::VARCHAR;
  } else if (mask_ & MASK_BOOLEAN) {
    type = FileColumnType::BOOLEAN;
  } else if (mask_ & MASK_BIGINT) {
    type = FileColumnType::BIGINT;
  } else if (mask_ & MASK_DOUBLE) {
    type = FileColumnType::DOUBLE;
  } else if (mask_ & MASK_DATE) {
    type = FileColumnType::DATE;
  } else if (mask_ & MASK_DATETIME) {
    type = FileColumnType::DATETIME;
  }
  return type;
}

int convert_text_value(const FileColumnType type, const char *str, const int64_t len,
                       FileValue &value, bool &ok)
{
  int ret = OB_SUCCESS;
  ok = true;
  switch (type) {
    case FileColumnType::BOOLEAN: {
      bool b = false;
      if ((ok = TextValueParser::parse_bool(str, len, b))) {
        value.set_int(b ? 1 : 0);
      }
      break;
    }
    case FileColumnType::BIGINT: {
      int64_t v = 0;
      bool out_of_range = false;
      if ((ok = (TextValueParser::parse_bigint(str, len, v, out_of_range) && !out_of_range))) {
        value.set_int(v);
      }
      break;
    }
    case FileColumnType::DOUBLE: {
      double d = 0;
      if ((ok = TextValueParser::parse_double(str, len, d))) {
        value.set_double(d);
      }
      break;
    }
    case FileColumnType::DATE: {
      int32_t days = 0;
      if ((ok = TextValueParser::parse_date(str, len, days))) {
        value.set_int(days);
      }
      break;
    }
    case FileColumnType::DATETIME: {
      int64_t usec = 0;
      if ((ok = TextValueParser::parse_date_or_datetime(str, len, usec))) {
        value.set_int(usec);
      }
      break;
    }
    case FileColumnType::VARCHAR: {
      value.set_string(ObString(static_cast<int32_t>(len), str));
      break;
    }
    default: {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("unexpected column type", K(ret), K(type));
    }
  }
  return ret;
}

// -------------------------------------------------------------- FileScanDesc

static const int64_t FILE_SCAN_DESC_VERSION = 1;

int FileScanDesc::serialize(ObIAllocator &allocator, ObString &out) const
{
  int ret = OB_SUCCESS;
  int64_t buf_len = 512 + func_name_.length() + path_.length() + secure_file_priv_.length()
                    + csv_.null_str_.length();
  for (int64_t i = 0; i < columns_.count(); ++i) {
    buf_len += 160 + columns_.at(i).name_.length() + columns_.at(i).source_name_.length();
  }
  char *buf = static_cast<char *>(allocator.alloc(buf_len));
  int64_t pos = 0;
  if (OB_ISNULL(buf)) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
    LOG_WARN("failed to allocate", K(ret), K(buf_len));
  }
  FS_ENCODE_INT(FILE_SCAN_DESC_VERSION);
  FS_ENCODE_INT(format_);
  FS_ENCODE_STR(func_name_);
  FS_ENCODE_STR(path_);
  FS_ENCODE_STR(secure_file_priv_);
  FS_ENCODE_INT(csv_.header_);
  FS_ENCODE_INT(csv_.delim_);
  FS_ENCODE_INT(csv_.quote_);
  FS_ENCODE_INT(csv_.escape_);
  FS_ENCODE_INT(csv_.has_null_str_);
  FS_ENCODE_STR(csv_.null_str_);
  FS_ENCODE_INT(csv_.ignore_errors_);
  FS_ENCODE_INT(max_line_size_);
  FS_ENCODE_INT(fingerprint_.dev_);
  FS_ENCODE_INT(fingerprint_.ino_);
  FS_ENCODE_INT(fingerprint_.size_);
  FS_ENCODE_INT(fingerprint_.mtime_ns_);
  FS_ENCODE_INT(row_count_);
  FS_ENCODE_INT(columns_.count());
  for (int64_t i = 0; OB_SUCC(ret) && i < columns_.count(); ++i) {
    const FileColumnDesc &c = columns_.at(i);
    FS_ENCODE_STR(c.name_);
    FS_ENCODE_INT(c.type_);
    FS_ENCODE_INT(c.nullable_);
    FS_ENCODE_INT(c.scale_);
    FS_ENCODE_INT(c.max_length_);
    FS_ENCODE_INT(c.source_idx_);
    FS_ENCODE_INT(c.source_);
    FS_ENCODE_INT(c.convert_);
    FS_ENCODE_INT(c.param_);
    FS_ENCODE_INT(c.type_length_);
    FS_ENCODE_STR(c.source_name_);
  }
  if (OB_SUCC(ret)) {
    out.assign_ptr(buf, static_cast<int32_t>(pos));
  }
  return ret;
}

int FileScanDesc::deserialize(const ObString &in)
{
  int ret = OB_SUCCESS;
  const char *buf = in.ptr();
  const int64_t data_len = in.length();
  int64_t pos = 0;
  int64_t version = 0;
  int64_t col_cnt = 0;
  columns_.reuse();
  FS_DECODE_INT(version, int64_t);
  if (OB_SUCC(ret) && FILE_SCAN_DESC_VERSION != version) {
    ret = OB_DESERIALIZE_ERROR;
    LOG_WARN("unexpected file scan desc version", K(ret), K(version));
  }
  FS_DECODE_INT(format_, FileFormat);
  FS_DECODE_STR(func_name_);
  FS_DECODE_STR(path_);
  FS_DECODE_STR(secure_file_priv_);
  FS_DECODE_INT(csv_.header_, bool);
  FS_DECODE_INT(csv_.delim_, char);
  FS_DECODE_INT(csv_.quote_, char);
  FS_DECODE_INT(csv_.escape_, char);
  FS_DECODE_INT(csv_.has_null_str_, bool);
  FS_DECODE_STR(csv_.null_str_);
  FS_DECODE_INT(csv_.ignore_errors_, bool);
  FS_DECODE_INT(max_line_size_, int64_t);
  FS_DECODE_INT(fingerprint_.dev_, int64_t);
  FS_DECODE_INT(fingerprint_.ino_, int64_t);
  FS_DECODE_INT(fingerprint_.size_, int64_t);
  FS_DECODE_INT(fingerprint_.mtime_ns_, int64_t);
  FS_DECODE_INT(row_count_, int64_t);
  FS_DECODE_INT(col_cnt, int64_t);
  if (OB_SUCC(ret) && (col_cnt < 0 || col_cnt > FILE_SQL_MAX_COLUMNS)) {
    ret = OB_DESERIALIZE_ERROR;
    LOG_WARN("invalid column count", K(ret), K(col_cnt));
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < col_cnt; ++i) {
    FileColumnDesc c;
    FS_DECODE_STR(c.name_);
    FS_DECODE_INT(c.type_, FileColumnType);
    FS_DECODE_INT(c.nullable_, bool);
    FS_DECODE_INT(c.scale_, int16_t);
    FS_DECODE_INT(c.max_length_, int64_t);
    FS_DECODE_INT(c.source_idx_, int32_t);
    FS_DECODE_INT(c.source_, FileValueSource);
    FS_DECODE_INT(c.convert_, FileValueConvert);
    FS_DECODE_INT(c.param_, int32_t);
    FS_DECODE_INT(c.type_length_, int32_t);
    FS_DECODE_STR(c.source_name_);
    if (OB_SUCC(ret) && OB_FAIL(columns_.push_back(c))) {
      LOG_WARN("push back failed", K(ret));
    }
  }
  return ret;
}

int FileScanDesc::find_column(const ObString &name, int64_t &idx) const
{
  int ret = OB_ENTRY_NOT_EXIST;
  idx = -1;
  for (int64_t i = 0; OB_ENTRY_NOT_EXIST == ret && i < columns_.count(); ++i) {
    if (0 == columns_.at(i).name_.case_compare(name)) {
      idx = i;
      ret = OB_SUCCESS;
    }
  }
  return ret;
}


// ------------------------------------------------------------------- file io

int pread_full(const int fd, char *buf, const int64_t len, const int64_t offset, int64_t &read_len)
{
  int ret = OB_SUCCESS;
  read_len = 0;
  while (OB_SUCC(ret) && read_len < len) {
    const ssize_t n = ::pread(fd, buf + read_len, len - read_len, offset + read_len);
    if (n < 0) {
      if (EINTR != errno) {
        ret = OB_IO_ERROR;
        LOG_WARN("pread failed", K(ret), K(errno), K(len), K(offset));
      }
    } else if (0 == n) {
      break;
    } else {
      read_len += n;
    }
  }
  return ret;
}

int open_input_file(const ObString &path, const FileFingerprint *expect, int &fd,
                    FileFingerprint &fingerprint)
{
  int ret = OB_SUCCESS;
  char path_buf[PATH_MAX + 1];
  fd = -1;
  if (path.empty() || path.length() > PATH_MAX) {
    ret = OB_INVALID_ARGUMENT;
    FORWARD_USER_ERROR_MSG(ret, "invalid file path '%.*s'", path.length(), path.ptr());
  } else {
    MEMCPY(path_buf, path.ptr(), path.length());
    path_buf[path.length()] = '\0';
    // O_NOFOLLOW: the path has been canonicalized and validated against
    // secure_file_priv, a symlink appearing here means the file was swapped.
    fd = ::open(path_buf, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
      if (ENOENT == errno) {
        ret = OB_FILE_NOT_EXIST;
        FORWARD_USER_ERROR_MSG(ret, "file '%.*s' does not exist", path.length(), path.ptr());
      } else if (ELOOP == errno) {
        ret = OB_INVALID_DATA;
        FORWARD_USER_ERROR_MSG(ret, "file '%.*s' has changed since the query was planned",
                               path.length(), path.ptr());
      } else {
        ret = OB_IO_ERROR;
        FORWARD_USER_ERROR_MSG(ret, "cannot open file '%.*s' (errno %d)",
                               path.length(), path.ptr(), errno);
      }
    } else {
      struct stat st;
      if (0 != ::fstat(fd, &st)) {
        ret = OB_IO_ERROR;
        LOG_WARN("fstat failed", K(ret), K(errno));
      } else if (!S_ISREG(st.st_mode)) {
        ret = OB_NOT_SUPPORTED;
        FORWARD_USER_ERROR_MSG(ret, "'%.*s' is not a regular file", path.length(), path.ptr());
      } else if (OB_FAIL(fingerprint.load_from_fd(fd))) {
      } else if (OB_NOT_NULL(expect) && expect->is_valid() && *expect != fingerprint) {
        ret = OB_INVALID_DATA;
        FORWARD_USER_ERROR_MSG(ret, "file '%.*s' has changed since the query was planned",
                               path.length(), path.ptr());
      }
      if (OB_FAIL(ret)) {
        ::close(fd);
        fd = -1;
      }
    }
  }
  return ret;
}

int check_path_in_secure_dir(const ObString &real_path, const ObString &secure_file_priv,
                             bool &allowed)
{
  int ret = OB_SUCCESS;
  allowed = false;
  char secure_buf[PATH_MAX + 1];
  char real_secure[PATH_MAX + 1];
  ObString priv = secure_file_priv;
  priv = priv.trim();
  if (priv.empty() || 0 == priv.case_compare("NULL")) {
    allowed = false;
  } else if (priv.length() > PATH_MAX) {
    allowed = false;
  } else {
    MEMCPY(secure_buf, priv.ptr(), priv.length());
    secure_buf[priv.length()] = '\0';
    struct stat st;
    if (NULL == ::realpath(secure_buf, real_secure)) {
      allowed = false;
    } else if (0 != ::stat(real_secure, &st) || !S_ISDIR(st.st_mode)) {
      allowed = false;
    } else {
      const int64_t dir_len = STRLEN(real_secure);
      if (1 == dir_len && '/' == real_secure[0]) {
        allowed = real_path.length() > 0 && '/' == real_path.ptr()[0];
      } else if (real_path.length() >= dir_len
                 && 0 == MEMCMP(real_path.ptr(), real_secure, dir_len)
                 && (real_path.length() == dir_len || '/' == real_path.ptr()[dir_len])) {
        allowed = true;
      }
    }
  }
  return ret;
}

int resolve_secure_path(const ObString &path, const ObString &secure_file_priv,
                        ObIAllocator &allocator, ObString &real_path)
{
  int ret = OB_SUCCESS;
  char path_buf[PATH_MAX + 1];
  char real_buf[PATH_MAX + 1];
  bool allowed = false;
  if (path.empty() || path.length() > PATH_MAX) {
    ret = OB_INVALID_ARGUMENT;
    FORWARD_USER_ERROR_MSG(ret, "invalid file path '%.*s'", path.length(), path.ptr());
  } else {
    MEMCPY(path_buf, path.ptr(), path.length());
    path_buf[path.length()] = '\0';
    if (NULL == ::realpath(path_buf, real_buf)) {
      ret = OB_FILE_NOT_EXIST;
      FORWARD_USER_ERROR_MSG(ret, "file '%.*s' does not exist", path.length(), path.ptr());
    } else if (OB_FAIL(check_path_in_secure_dir(ObString(real_buf), secure_file_priv, allowed))) {
    } else if (!allowed) {
      ret = OB_ERR_NO_PRIVILEGE;
      FORWARD_USER_ERROR_MSG(ret, "access to '%.*s' is denied, the file must be inside secure_file_priv",
                             path.length(), path.ptr());
    } else if (OB_FAIL(ob_write_string(allocator, ObString(real_buf), real_path))) {
      LOG_WARN("failed to copy path", K(ret));
    }
  }
  return ret;
}

int check_file_unchanged(const int fd, const ObString &path, const FileFingerprint &expect)
{
  int ret = OB_SUCCESS;
  FileFingerprint current;
  if (!expect.is_valid()) {
  } else if (OB_FAIL(current.load_from_fd(fd))) {
  } else if (current != expect) {
    ret = OB_INVALID_DATA;
    FORWARD_USER_ERROR_MSG(ret, "file '%.*s' has changed during the scan",
                           path.length(), path.ptr());
  }
  return ret;
}

}  // namespace file_sql
}  // namespace sql
}  // namespace oceanbase
