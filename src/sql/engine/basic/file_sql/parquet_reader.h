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
// Self contained, streaming Parquet reader for flat schemas.
//
// Supported: Thrift compact footer / page headers, data page v1 and v2,
// dictionary pages, PLAIN / PLAIN_DICTIONARY / RLE_DICTIONARY / RLE(boolean)
// encodings, RLE / bit-packed definition levels, UNCOMPRESSED / SNAPPY /
// GZIP / ZSTD codecs. Column chunks are read page by page, so memory is
// bounded by the largest page of the projected columns.

#pragma once

#include "lib/allocator/ob_malloc.h"
#include "sql/engine/basic/file_sql/file_scanner.h"

namespace oceanbase
{
namespace sql
{
namespace file_sql
{

namespace parquet
{
enum PhysicalType
{
  PT_BOOLEAN = 0,
  PT_INT32 = 1,
  PT_INT64 = 2,
  PT_INT96 = 3,
  PT_FLOAT = 4,
  PT_DOUBLE = 5,
  PT_BYTE_ARRAY = 6,
  PT_FIXED_LEN_BYTE_ARRAY = 7,
};

enum Codec
{
  CODEC_UNCOMPRESSED = 0,
  CODEC_SNAPPY = 1,
  CODEC_GZIP = 2,
  CODEC_LZO = 3,
  CODEC_BROTLI = 4,
  CODEC_LZ4 = 5,
  CODEC_ZSTD = 6,
  CODEC_LZ4_RAW = 7,
};

enum Encoding
{
  ENC_PLAIN = 0,
  ENC_PLAIN_DICTIONARY = 2,
  ENC_RLE = 3,
  ENC_BIT_PACKED = 4,
  ENC_RLE_DICTIONARY = 8,
};

enum PageType
{
  PAGE_DATA = 0,
  PAGE_INDEX = 1,
  PAGE_DICTIONARY = 2,
  PAGE_DATA_V2 = 3,
};

// Thrift compact protocol decoder over a memory buffer. Every read is bounds
// checked; malformed input yields OB_INVALID_DATA instead of crashing.
class ThriftCompactReader
{
public:
  static const int8_t T_STOP = 0;
  static const int8_t T_BOOL_TRUE = 1;
  static const int8_t T_BOOL_FALSE = 2;
  static const int8_t T_BYTE = 3;
  static const int8_t T_I16 = 4;
  static const int8_t T_I32 = 5;
  static const int8_t T_I64 = 6;
  static const int8_t T_DOUBLE = 7;
  static const int8_t T_BINARY = 8;
  static const int8_t T_LIST = 9;
  static const int8_t T_SET = 10;
  static const int8_t T_MAP = 11;
  static const int8_t T_STRUCT = 12;

  ThriftCompactReader(const char *buf, const int64_t len)
    : buf_(reinterpret_cast<const uint8_t *>(buf)), len_(len), pos_(0), depth_(0) {}
  int read_varint(uint64_t &v);
  int read_i32(int32_t &v);
  int read_i64(int64_t &v);
  int read_binary(common::ObString &v);
  int read_byte(int8_t &v);
  int read_double(double &v);
  // field_type T_STOP marks the end of a struct; `last_id` tracks delta ids
  int read_field_header(int16_t &last_id, int16_t &field_id, int8_t &field_type, bool &bool_value);
  int read_list_header(int8_t &elem_type, int64_t &size);
  int skip(const int8_t type);
  int64_t get_pos() const { return pos_; }
private:
  int skip_struct();
  const uint8_t *buf_;
  int64_t len_;
  int64_t pos_;
  int64_t depth_;
};

struct LogicalTypeInfo
{
  LogicalTypeInfo() : kind_(0), unit_(0), scale_(0), bit_width_(0), is_signed_(true) {}
  int16_t kind_;     // LogicalType union field id, 0 when absent
  int16_t unit_;     // TimeUnit union field id for TIME / TIMESTAMP
  int32_t scale_;
  int8_t bit_width_;
  bool is_signed_;
};

struct SchemaElement
{
  SchemaElement() : type_(-1), type_length_(0), repetition_(0), name_(), num_children_(0),
                    converted_type_(-1), scale_(0), precision_(0), logical_() {}
  int32_t type_;
  int32_t type_length_;
  int32_t repetition_;
  common::ObString name_;
  int32_t num_children_;
  int32_t converted_type_;
  int32_t scale_;
  int32_t precision_;
  LogicalTypeInfo logical_;
};

struct ColumnChunkMeta
{
  ColumnChunkMeta() : type_(-1), codec_(0), num_values_(0), data_page_offset_(-1),
                      dictionary_page_offset_(-1), total_compressed_size_(0), path_depth_(0) {}
  int32_t type_;
  int32_t codec_;
  int64_t num_values_;
  int64_t data_page_offset_;
  int64_t dictionary_page_offset_;
  int64_t total_compressed_size_;
  int64_t path_depth_;
};

struct RowGroupMeta
{
  RowGroupMeta() : num_rows_(0), columns_() {}
  int64_t num_rows_;
  common::ObSEArray<ColumnChunkMeta, 8> columns_;
};

struct FileMeta
{
  FileMeta() : num_rows_(0), schema_(), row_groups_() {}
  int64_t num_rows_;
  common::ObSEArray<SchemaElement, 16> schema_;
  common::ObSEArray<RowGroupMeta, 4> row_groups_;
};

struct PageHeader
{
  PageHeader() : type_(-1), uncompressed_size_(0), compressed_size_(0), num_values_(0),
                 encoding_(0), def_level_encoding_(ENC_RLE), num_nulls_(0),
                 def_levels_len_(0), rep_levels_len_(0), is_compressed_(true) {}
  int32_t type_;
  int32_t uncompressed_size_;
  int32_t compressed_size_;
  int32_t num_values_;
  int32_t encoding_;
  int32_t def_level_encoding_;
  int32_t num_nulls_;
  int32_t def_levels_len_;
  int32_t rep_levels_len_;
  bool is_compressed_;
};

int parse_file_meta(const char *buf, const int64_t len, FileMeta &meta);
int parse_page_header(const char *buf, const int64_t len, PageHeader &header, int64_t &header_len);

// Raw (unframed) snappy block decompression.
int snappy_uncompressed_length(const char *src, const int64_t src_len, int64_t &len);
int snappy_decompress(const char *src, const int64_t src_len, char *dst, const int64_t dst_len);

// RLE / bit-packed hybrid decoder (levels, dictionary indexes, booleans).
class RleBitPackedDecoder
{
public:
  RleBitPackedDecoder() : data_(nullptr), len_(0), pos_(0), bit_width_(0), rle_left_(0),
                          rle_value_(0), packed_left_(0), packed_bit_pos_(0) {}
  int init(const uint8_t *data, const int64_t len, const int32_t bit_width);
  int next(uint32_t &v);
private:
  int read_header();
  const uint8_t *data_;
  int64_t len_;
  int64_t pos_;
  int32_t bit_width_;
  int64_t rle_left_;
  uint32_t rle_value_;
  int64_t packed_left_;
  int64_t packed_bit_pos_;  // absolute bit position inside data_
};
}  // namespace parquet

// Physical value of one column slot before semantic conversion.
struct ParquetRawValue
{
  ParquetRawValue() : is_null_(true), int_(0), double_(0), str_() {}
  bool is_null_;
  int64_t int_;
  double double_;
  common::ObString str_;
};

// Streams the values of one column chunk page by page.
class ParquetColumnReader
{
public:
  ParquetColumnReader();
  ~ParquetColumnReader() { reset(); }
  int init(const int fd, const common::ObString &path, const int64_t file_size,
           const parquet::ColumnChunkMeta &meta, const parquet::SchemaElement &schema,
           const int16_t max_def_level);
  int next(ParquetRawValue &value);
  void reset();
private:
  int load_next_page();
  int read_bytes(const int64_t offset, const int64_t len, FileBuffer &buf);
  int decompress(const char *src, const int64_t src_len, const int64_t dst_len, FileBuffer &dst);
  int load_dictionary(const parquet::PageHeader &header, const char *data, const int64_t len);
  int decode_plain(ParquetRawValue &value);
  int read_plain_at(const uint8_t *data, const int64_t len, int64_t &pos, int64_t &bit_pos,
                    ParquetRawValue &value);
  int fail(const char *msg);
  int fd_;
  common::ObString path_;
  int64_t file_size_;
  int32_t type_;
  int32_t type_length_;
  int32_t codec_;
  int16_t max_def_level_;
  int64_t next_page_offset_;
  int64_t chunk_end_;
  int64_t values_left_in_chunk_;
  // current data page
  FileBuffer raw_page_;
  FileBuffer page_;
  const uint8_t *values_;
  int64_t values_len_;
  int64_t values_pos_;
  int64_t values_bit_pos_;
  int64_t page_values_left_;
  int32_t page_encoding_;
  bool has_def_levels_;
  parquet::RleBitPackedDecoder def_decoder_;
  parquet::RleBitPackedDecoder value_decoder_;  // dictionary indexes or RLE booleans
  // dictionary
  bool has_dictionary_;
  FileBuffer dict_data_;
  common::ObSEArray<ParquetRawValue, 64> dict_;
  FileBuffer header_buf_;
  common::ObMalloc decompress_allocator_;
};

class ParquetScanner : public FileScanner
{
public:
  ParquetScanner() : desc_(nullptr), fd_(-1), meta_(), meta_buf_(), projected_(), readers_(),
                     row_group_idx_(-1), rows_left_in_group_(0) {}
  virtual ~ParquetScanner() { close(); }
  virtual int open(const FileScanDesc &desc, const common::ObIArray<bool> &projected) override;
  virtual int rescan() override;
  virtual int get_next_row(common::ObIArray<FileValue> &row) override;
  virtual void close() override;
  // reads and validates the footer
  static int load_meta(const int fd, const common::ObString &path, const int64_t file_size,
                       FileBuffer &meta_buf, parquet::FileMeta &meta);
private:
  int open_row_group(const int64_t idx);
  int convert_value(const FileColumnDesc &col, const ParquetRawValue &raw, FileValue &value);
  void reset_readers();
  const FileScanDesc *desc_;
  int fd_;
  parquet::FileMeta meta_;
  FileBuffer meta_buf_;
  common::ObSEArray<bool, 16> projected_;
  common::ObSEArray<ParquetColumnReader *, 16> readers_;
  int64_t row_group_idx_;
  int64_t rows_left_in_group_;
};

int infer_parquet_schema(common::ObIAllocator &allocator, FileScanDesc &desc);

}  // namespace file_sql
}  // namespace sql
}  // namespace oceanbase
