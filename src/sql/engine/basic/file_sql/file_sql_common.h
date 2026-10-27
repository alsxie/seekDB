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
// Shared building blocks of "file as database" (read_csv / read_json /
// read_parquet / glob): logical column types, deterministic type inference,
// strict text value parsing, file fingerprints, reusable buffers and the
// serializable scan descriptor handed from the resolver to the operator.

#pragma once

#include "lib/allocator/ob_allocator.h"
#include "lib/container/ob_se_array.h"
#include "lib/string/ob_string.h"
#include "lib/utility/ob_print_utils.h"
#include "lib/utility/serialization.h"

// Varint helpers shared by the descriptors serialized into plans. They expect
// `buf`, `buf_len` / `data_len`, `pos` and `ret` in scope.
#define FS_ENCODE_INT(v)                                                          \
  if (OB_SUCC(ret) && OB_FAIL(serialization::encode_vi64(buf, buf_len, pos,      \
                                                         static_cast<int64_t>(v)))) { \
    LOG_WARN("encode failed", K(ret));                                            \
  }
#define FS_ENCODE_STR(s)                                                          \
  if (OB_SUCC(ret) && OB_FAIL(serialization::encode_vstr(buf, buf_len, pos, (s).ptr(), (s).length()))) { \
    LOG_WARN("encode failed", K(ret));                                            \
  }
#define FS_DECODE_INT(v, T)                                                       \
  if (OB_SUCC(ret)) {                                                             \
    int64_t tmp_v = 0;                                                            \
    if (OB_FAIL(serialization::decode_vi64(buf, data_len, pos, &tmp_v))) {        \
      LOG_WARN("decode failed", K(ret));                                          \
    } else {                                                                      \
      v = static_cast<T>(tmp_v);                                                  \
    }                                                                             \
  }
#define FS_DECODE_STR(s)                                                          \
  if (OB_SUCC(ret)) {                                                             \
    int64_t tmp_len = 0;                                                          \
    const char *tmp_ptr = serialization::decode_vstr(buf, data_len, pos, &tmp_len); \
    if (OB_ISNULL(tmp_ptr)) {                                                     \
      ret = OB_DESERIALIZE_ERROR;                                                 \
      LOG_WARN("decode failed", K(ret));                                          \
    } else {                                                                      \
      s.assign_ptr(tmp_ptr, static_cast<int32_t>(tmp_len));                       \
    }                                                                             \
  }

namespace oceanbase
{
namespace sql
{
namespace file_sql
{

enum class FileFormat : int8_t
{
  INVALID = 0,
  CSV = 1,
  JSONL = 2,
  PARQUET = 3,
  GLOB = 4,
};

// Logical column types exposed to SQL. Declaration order is the inference
// preference order: the first type that can represent every non-NULL value of
// a column wins, VARCHAR always can.
enum class FileColumnType : int8_t
{
  NULL_TYPE = 0,
  BOOLEAN = 1,
  BIGINT = 2,
  DOUBLE = 3,
  DATE = 4,
  DATETIME = 5,
  VARCHAR = 6,
};

const char *get_file_format_name(FileFormat format);
const char *get_file_column_type_name(FileColumnType type);

// Resource limits. They bound the memory of a single record / page so that a
// malformed or hostile file cannot exhaust the server.
static const int64_t FILE_SQL_DEFAULT_MAX_LINE_SIZE = 2LL * 1024 * 1024;
static const int64_t FILE_SQL_MAX_LINE_SIZE_LIMIT = 64LL * 1024 * 1024;
static const int64_t FILE_SQL_MAX_COLUMNS = 4096;
static const int64_t FILE_SQL_MAX_COLUMN_NAME_LEN = 256;
static const int64_t FILE_SQL_MAX_PARQUET_FOOTER_SIZE = 64LL * 1024 * 1024;
static const int64_t FILE_SQL_MAX_PARQUET_PAGE_SIZE = 256LL * 1024 * 1024;
static const int64_t FILE_SQL_MAX_GLOB_FILES = 100000;
static const int64_t FILE_SQL_MAX_VARCHAR_LENGTH = 262144;  // characters
static const int64_t FILE_SQL_READ_BLOCK_SIZE = 256LL * 1024;

// Growable heap buffer that is reused across records / pages. Memory comes
// from ob_malloc so it is released as soon as the scan ends, independent of
// the arena that owns the operator.
class FileBuffer
{
public:
  FileBuffer() : buf_(nullptr), len_(0), cap_(0) {}
  ~FileBuffer() { reset(); }
  int reserve(const int64_t cap);
  int append(const char *data, const int64_t len);
  int push_back(const char c)
  {
    int ret = common::OB_SUCCESS;
    if (OB_UNLIKELY(len_ >= cap_) && OB_FAIL(reserve(len_ + 1))) {
    } else {
      buf_[len_++] = c;
    }
    return ret;
  }
  int resize(const int64_t len);
  void reuse() { len_ = 0; }
  void reset();
  char *ptr() { return buf_; }
  const char *ptr() const { return buf_; }
  int64_t length() const { return len_; }
  int64_t capacity() const { return cap_; }
  common::ObString str() const { return common::ObString(static_cast<int32_t>(len_), buf_); }
private:
  DISALLOW_COPY_AND_ASSIGN(FileBuffer);
  char *buf_;
  int64_t len_;
  int64_t cap_;
};

// Identity of a file version. Captured when the schema is inferred and checked
// again whenever the file is (re)opened or reaches EOF, so that a file that is
// replaced or modified between planning and execution is reported instead of
// silently producing rows that do not match the inferred schema.
struct FileFingerprint
{
  FileFingerprint() : dev_(0), ino_(0), size_(-1), mtime_ns_(0) {}
  int load_from_fd(const int fd);
  bool is_valid() const { return size_ >= 0; }
  bool operator==(const FileFingerprint &other) const
  {
    return dev_ == other.dev_ && ino_ == other.ino_ && size_ == other.size_
           && mtime_ns_ == other.mtime_ns_;
  }
  bool operator!=(const FileFingerprint &other) const { return !(*this == other); }
  TO_STRING_KV(K_(dev), K_(ino), K_(size), K_(mtime_ns));
  int64_t dev_;
  int64_t ino_;
  int64_t size_;
  int64_t mtime_ns_;
};

// Strict, locale independent parsers for textual values. They never accept
// leading / trailing spaces so that inference is deterministic.
class TextValueParser
{
public:
  static bool parse_bool(const char *str, const int64_t len, bool &value);
  // returns true when the text is an integer literal; overflow is reported by
  // `out_of_range` (value is then meaningless).
  static bool parse_bigint(const char *str, const int64_t len, int64_t &value, bool &out_of_range);
  static bool parse_double(const char *str, const int64_t len, double &value);
  static bool parse_date(const char *str, const int64_t len, int32_t &days);
  // accepts "YYYY-MM-DD[ T]HH:MM:SS[.f{1,6}]"; scale is the number of
  // fractional digits.
  static bool parse_datetime(const char *str, const int64_t len, int64_t &usec, int16_t &scale);
  // DATETIME columns also accept plain dates (widened DATE values).
  static bool parse_date_or_datetime(const char *str, const int64_t len, int64_t &usec);
  static bool is_valid_utf8(const char *str, const int64_t len);
  static int64_t utf8_char_length(const char *str, const int64_t len);
  static int64_t days_from_civil(int64_t year, int64_t month, int64_t day);
  static bool is_valid_date(int64_t year, int64_t month, int64_t day);
};

static const int64_t USECS_PER_DAY = 86400LL * 1000000LL;

// Per column inference state, O(1) memory regardless of the file size.
class ColumnInferState
{
public:
  static const uint8_t MASK_BOOLEAN = 1 << 1;
  static const uint8_t MASK_BIGINT = 1 << 2;
  static const uint8_t MASK_DOUBLE = 1 << 3;
  static const uint8_t MASK_DATE = 1 << 4;
  static const uint8_t MASK_DATETIME = 1 << 5;
  static const uint8_t MASK_VARCHAR = 1 << 6;
  static const uint8_t MASK_ALL = MASK_BOOLEAN | MASK_BIGINT | MASK_DOUBLE | MASK_DATE
                                  | MASK_DATETIME | MASK_VARCHAR;

  ColumnInferState() : mask_(MASK_ALL), has_null_(false), has_value_(false),
                       datetime_scale_(0), max_char_length_(0) {}
  void add_null() { has_null_ = true; }
  // untyped text (CSV fields)
  void add_text(const char *str, const int64_t len);
  // typed JSON scalars
  void add_json_bool();
  void add_json_number(const char *str, const int64_t len);
  void add_json_string(const char *str, const int64_t len);
  // A missing JSON key behaves like NULL.
  void add_missing() { has_null_ = true; }
  FileColumnType get_type() const;
  bool has_null() const { return has_null_; }
  bool has_value() const { return has_value_; }
  int16_t get_datetime_scale() const { return datetime_scale_; }
  int64_t get_max_char_length() const { return max_char_length_; }
  TO_STRING_KV(K_(mask), K_(has_null), K_(has_value), K_(datetime_scale), K_(max_char_length));
private:
  void add_mask(const uint8_t mask, const char *str, const int64_t len);
  uint8_t mask_;
  bool has_null_;
  bool has_value_;
  int16_t datetime_scale_;
  int64_t max_char_length_;
};

// How a physical source value is turned into the logical column value.
enum class FileValueSource : int8_t
{
  TEXT = 0,               // CSV text / JSON scalar converted by logical type
  PARQUET_BOOLEAN = 1,
  PARQUET_INT32 = 2,
  PARQUET_INT64 = 3,
  PARQUET_FLOAT = 4,
  PARQUET_DOUBLE = 5,
  PARQUET_BYTE_ARRAY = 6,
  PARQUET_FIXED_BYTE_ARRAY = 7,
  PARQUET_INT96 = 8,
};

// Semantic conversion applied on top of the physical value.
enum class FileValueConvert : int8_t
{
  NONE = 0,
  DATE_DAYS = 1,          // int32 days since epoch
  TIMESTAMP_MILLIS = 2,
  TIMESTAMP_MICROS = 3,
  TIMESTAMP_NANOS = 4,
  DECIMAL = 5,            // unscaled integer with param_ = scale
  UNSIGNED = 6,           // unsigned integer stored in a signed physical type
  INT96_TIMESTAMP = 7,
};

struct FileColumnDesc
{
  FileColumnDesc()
    : name_(), type_(FileColumnType::VARCHAR), nullable_(true), scale_(0),
      max_length_(0), source_idx_(-1), source_(FileValueSource::TEXT),
      convert_(FileValueConvert::NONE), param_(0), type_length_(0), source_name_() {}
  TO_STRING_KV(K_(name), K_(source_name), K_(type), K_(nullable), K_(scale), K_(max_length), K_(source_idx),
               K_(source), K_(convert), K_(param), K_(type_length));
  common::ObString name_;
  FileColumnType type_;
  bool nullable_;
  int16_t scale_;           // fractional seconds digits of DATETIME
  int64_t max_length_;      // max characters of VARCHAR
  int32_t source_idx_;      // CSV field index / Parquet leaf index
  FileValueSource source_;
  FileValueConvert convert_;
  int32_t param_;           // decimal scale
  int32_t type_length_;     // fixed byte array length
  common::ObString source_name_;  // JSON key the column is read from
};

struct CsvOptions
{
  CsvOptions() : header_(true), delim_(','), quote_('"'), escape_('"'), has_null_str_(false),
                 null_str_(), ignore_errors_(false) {}
  TO_STRING_KV(K_(header), K_(delim), K_(quote), K_(escape), K_(has_null_str), K_(null_str),
               K_(ignore_errors));
  bool header_;
  char delim_;
  char quote_;     // '\0' disables quoting
  char escape_;    // equal to quote_ means RFC 4180 doubled quotes
  bool has_null_str_;
  common::ObString null_str_;
  bool ignore_errors_;
};

struct FileScanDesc
{
  FileScanDesc() : format_(FileFormat::INVALID), func_name_(), path_(), secure_file_priv_(),
                   csv_(), max_line_size_(FILE_SQL_DEFAULT_MAX_LINE_SIZE), fingerprint_(),
                   row_count_(-1), columns_() {}
  int serialize(common::ObIAllocator &allocator, common::ObString &out) const;
  // Strings of the result point into `in`, which must outlive the descriptor.
  int deserialize(const common::ObString &in);
  int find_column(const common::ObString &name, int64_t &idx) const;
  TO_STRING_KV(K_(format), K_(func_name), K_(path), K_(csv), K_(max_line_size), K_(fingerprint),
               K_(row_count), K_(columns));
  FileFormat format_;
  common::ObString func_name_;
  common::ObString path_;              // canonical path, or the glob pattern
  common::ObString secure_file_priv_;  // used by glob to filter results at runtime
  CsvOptions csv_;
  int64_t max_line_size_;
  FileFingerprint fingerprint_;
  int64_t row_count_;
  common::ObSEArray<FileColumnDesc, 16> columns_;
};

// One logical value produced by a scan.
struct FileValue
{
  FileValue() : is_null_(true), int_(0), double_(0), str_() {}
  void set_null() { is_null_ = true; }
  void set_int(const int64_t v) { is_null_ = false; int_ = v; }
  void set_double(const double v) { is_null_ = false; double_ = v; }
  void set_string(const common::ObString &v) { is_null_ = false; str_ = v; }
  TO_STRING_KV(K_(is_null), K_(int), K_(double), K_(str));
  bool is_null_;
  int64_t int_;      // BOOLEAN (0/1), BIGINT, DATE (days), DATETIME (usec)
  double double_;
  common::ObString str_;
};

// Converts CSV text / JSON text into the logical value of `type`.
int convert_text_value(const FileColumnType type, const char *str, const int64_t len,
                       FileValue &value, bool &ok);

// Reads a whole file region with retries on EINTR / short reads.
int pread_full(const int fd, char *buf, const int64_t len, const int64_t offset, int64_t &read_len);

// Opens a regular file read only and returns its fingerprint. When `expect`
// is valid the fingerprint must match, otherwise OB_FILE_NOT_EXIST /
// OB_INVALID_DATA is returned with a user visible message.
int open_input_file(const common::ObString &path, const FileFingerprint *expect,
                    int &fd, FileFingerprint &fingerprint);
// Canonicalizes `path` (resolving symlinks and "..") into `real_path` and
// checks it lies inside secure_file_priv on a directory boundary.
int resolve_secure_path(const common::ObString &path, const common::ObString &secure_file_priv,
                        common::ObIAllocator &allocator, common::ObString &real_path);
// `real_path` must already be canonical.
int check_path_in_secure_dir(const common::ObString &real_path,
                             const common::ObString &secure_file_priv, bool &allowed);
int check_file_unchanged(const int fd, const common::ObString &path, const FileFingerprint &expect);

}  // namespace file_sql
}  // namespace sql
}  // namespace oceanbase
