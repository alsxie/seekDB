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

#include "sql/engine/basic/file_sql/parquet_reader.h"

#include <cmath>
#include <cstdio>
#include <new>
#include <unistd.h>

#include "lib/compress/ob_compress_util.h"
#include "lib/compress/zstd_1_3_8/ob_zstd_wrapper.h"
#include "lib/oblog/ob_log_module.h"

namespace oceanbase
{
using namespace common;
namespace sql
{
namespace file_sql
{
namespace parquet
{

// ------------------------------------------------------- ThriftCompactReader

static const int64_t THRIFT_MAX_DEPTH = 64;
static const int64_t THRIFT_MAX_LIST_SIZE = 16 * 1024 * 1024;

int ThriftCompactReader::read_varint(uint64_t &v)
{
  int ret = OB_SUCCESS;
  v = 0;
  int shift = 0;
  bool done = false;
  while (OB_SUCC(ret) && !done) {
    if (pos_ >= len_ || shift > 63) {
      ret = OB_INVALID_DATA;
      LOG_WARN("invalid thrift varint", K(ret), K(pos_), K(len_));
    } else {
      const uint8_t b = buf_[pos_++];
      v |= static_cast<uint64_t>(b & 0x7F) << shift;
      shift += 7;
      done = 0 == (b & 0x80);
    }
  }
  return ret;
}

int ThriftCompactReader::read_i32(int32_t &v)
{
  int ret = OB_SUCCESS;
  uint64_t u = 0;
  if (OB_SUCC(read_varint(u))) {
    const uint32_t z = static_cast<uint32_t>(u);
    v = static_cast<int32_t>((z >> 1) ^ (~(z & 1) + 1));
  }
  return ret;
}

int ThriftCompactReader::read_i64(int64_t &v)
{
  int ret = OB_SUCCESS;
  uint64_t u = 0;
  if (OB_SUCC(read_varint(u))) {
    v = static_cast<int64_t>((u >> 1) ^ (~(u & 1) + 1));
  }
  return ret;
}

int ThriftCompactReader::read_binary(ObString &v)
{
  int ret = OB_SUCCESS;
  uint64_t n = 0;
  if (OB_FAIL(read_varint(n))) {
  } else if (n > static_cast<uint64_t>(len_ - pos_)) {
    ret = OB_INVALID_DATA;
    LOG_WARN("thrift binary out of bounds", K(ret), K(n), K(pos_), K(len_));
  } else {
    v.assign_ptr(reinterpret_cast<const char *>(buf_ + pos_), static_cast<int32_t>(n));
    pos_ += static_cast<int64_t>(n);
  }
  return ret;
}

int ThriftCompactReader::read_byte(int8_t &v)
{
  int ret = OB_SUCCESS;
  if (pos_ >= len_) {
    ret = OB_INVALID_DATA;
  } else {
    v = static_cast<int8_t>(buf_[pos_++]);
  }
  return ret;
}

int ThriftCompactReader::read_double(double &v)
{
  int ret = OB_SUCCESS;
  if (pos_ + 8 > len_) {
    ret = OB_INVALID_DATA;
  } else {
    MEMCPY(&v, buf_ + pos_, 8);
    pos_ += 8;
  }
  return ret;
}

int ThriftCompactReader::read_field_header(int16_t &last_id, int16_t &field_id, int8_t &field_type,
                                           bool &bool_value)
{
  int ret = OB_SUCCESS;
  int8_t b = 0;
  if (OB_FAIL(read_byte(b))) {
    LOG_WARN("truncated thrift struct", K(ret));
  } else {
    field_type = static_cast<int8_t>(b & 0x0F);
    if (T_STOP != field_type) {
      const int16_t delta = static_cast<int16_t>((static_cast<uint8_t>(b) >> 4) & 0x0F);
      if (0 == delta) {
        int32_t id = 0;
        if (OB_FAIL(read_i32(id))) {
        } else {
          field_id = static_cast<int16_t>(id);
        }
      } else {
        field_id = static_cast<int16_t>(last_id + delta);
      }
      last_id = field_id;
      bool_value = (T_BOOL_TRUE == field_type);
      if (field_type > T_STRUCT) {
        ret = OB_INVALID_DATA;
        LOG_WARN("invalid thrift field type", K(ret), K(field_type));
      }
    }
  }
  return ret;
}

int ThriftCompactReader::read_list_header(int8_t &elem_type, int64_t &size)
{
  int ret = OB_SUCCESS;
  int8_t b = 0;
  if (OB_FAIL(read_byte(b))) {
  } else {
    elem_type = static_cast<int8_t>(b & 0x0F);
    size = (static_cast<uint8_t>(b) >> 4) & 0x0F;
    if (15 == size) {
      uint64_t n = 0;
      if (OB_FAIL(read_varint(n))) {
      } else {
        size = static_cast<int64_t>(n);
      }
    }
    // every element takes at least one byte (an empty struct is one STOP)
    if (OB_SUCC(ret) && (size < 0 || size > THRIFT_MAX_LIST_SIZE || size > len_ - pos_)) {
      ret = OB_INVALID_DATA;
      LOG_WARN("thrift list too large", K(ret), K(size));
    }
  }
  return ret;
}

int ThriftCompactReader::skip_struct()
{
  int ret = OB_SUCCESS;
  int16_t last_id = 0;
  if (++depth_ > THRIFT_MAX_DEPTH) {
    ret = OB_INVALID_DATA;
    LOG_WARN("thrift nesting too deep", K(ret));
  }
  while (OB_SUCC(ret)) {
    int16_t fid = 0;
    int8_t ftype = 0;
    bool bv = false;
    if (OB_FAIL(read_field_header(last_id, fid, ftype, bv))) {
    } else if (T_STOP == ftype) {
      break;
    } else if (OB_FAIL(skip(ftype))) {
    }
  }
  --depth_;
  return ret;
}

int ThriftCompactReader::skip(const int8_t type)
{
  int ret = OB_SUCCESS;
  switch (type) {
    case T_BOOL_TRUE:
    case T_BOOL_FALSE:
      break;  // value is in the field header
    case T_BYTE: {
      int8_t v = 0;
      ret = read_byte(v);
      break;
    }
    case T_I16:
    case T_I32:
    case T_I64: {
      uint64_t v = 0;
      ret = read_varint(v);
      break;
    }
    case T_DOUBLE: {
      double v = 0;
      ret = read_double(v);
      break;
    }
    case T_BINARY: {
      ObString v;
      ret = read_binary(v);
      break;
    }
    case T_LIST:
    case T_SET: {
      int8_t elem = 0;
      int64_t size = 0;
      if (OB_FAIL(read_list_header(elem, size))) {
      } else if (++depth_ > THRIFT_MAX_DEPTH) {
        ret = OB_INVALID_DATA;
      }
      for (int64_t i = 0; OB_SUCC(ret) && i < size; ++i) {
        if (T_BOOL_TRUE == elem || T_BOOL_FALSE == elem) {
          int8_t v = 0;
          ret = read_byte(v);
        } else {
          ret = skip(elem);
        }
      }
      --depth_;
      break;
    }
    case T_MAP: {
      uint64_t size = 0;
      int8_t kv = 0;
      if (OB_FAIL(read_varint(size))) {
      } else if (size > static_cast<uint64_t>(len_ - pos_)) {
        ret = OB_INVALID_DATA;
      } else if (size > 0 && OB_FAIL(read_byte(kv))) {
      } else if (++depth_ > THRIFT_MAX_DEPTH) {
        ret = OB_INVALID_DATA;
      }
      const int8_t kt = static_cast<int8_t>((static_cast<uint8_t>(kv) >> 4) & 0x0F);
      const int8_t vt = static_cast<int8_t>(kv & 0x0F);
      for (uint64_t i = 0; OB_SUCC(ret) && i < size; ++i) {
        if (OB_FAIL(skip(kt))) {
        } else if (OB_FAIL(skip(vt))) {
        }
      }
      --depth_;
      break;
    }
    case T_STRUCT:
      ret = skip_struct();
      break;
    default:
      ret = OB_INVALID_DATA;
      LOG_WARN("unknown thrift type", K(ret), K(type));
  }
  return ret;
}

// Helpers reading a typed field, skipping it when the wire type differs.
static int read_i32_field(ThriftCompactReader &r, const int8_t ftype, int32_t &v)
{
  return ThriftCompactReader::T_I32 == ftype ? r.read_i32(v) : r.skip(ftype);
}

static int read_i64_field(ThriftCompactReader &r, const int8_t ftype, int64_t &v)
{
  int ret = OB_SUCCESS;
  if (ThriftCompactReader::T_I64 == ftype) {
    ret = r.read_i64(v);
  } else if (ThriftCompactReader::T_I32 == ftype) {
    int32_t v32 = 0;
    if (OB_SUCC(r.read_i32(v32))) {
      v = v32;
    }
  } else {
    ret = r.skip(ftype);
  }
  return ret;
}

#define THRIFT_STRUCT_LOOP(reader, body)                                         \
  {                                                                             \
    int16_t last_id_ = 0;                                                       \
    while (OB_SUCC(ret)) {                                                      \
      int16_t fid = 0;                                                          \
      int8_t ftype = 0;                                                         \
      bool bval = false;                                                        \
      if (OB_FAIL((reader).read_field_header(last_id_, fid, ftype, bval))) {    \
      } else if (ThriftCompactReader::T_STOP == ftype) {                        \
        break;                                                                  \
      } else {                                                                  \
        body                                                                    \
      }                                                                         \
    }                                                                           \
  }

// LogicalType is a union: the set field id identifies the kind.
static int parse_logical_type(ThriftCompactReader &r, LogicalTypeInfo &info)
{
  int ret = OB_SUCCESS;
  THRIFT_STRUCT_LOOP(r, {
    if (ThriftCompactReader::T_STRUCT != ftype) {
      ret = r.skip(ftype);
    } else {
      info.kind_ = fid;
      if (5 == fid) {  // DECIMAL
        THRIFT_STRUCT_LOOP(r, {
          if (1 == fid) { ret = read_i32_field(r, ftype, info.scale_); }
          else { ret = r.skip(ftype); }
        });
      } else if (7 == fid || 8 == fid) {  // TIME / TIMESTAMP
        THRIFT_STRUCT_LOOP(r, {
          if (2 == fid && ThriftCompactReader::T_STRUCT == ftype) {
            THRIFT_STRUCT_LOOP(r, {
              info.unit_ = fid;
              ret = r.skip(ftype);
            });
          } else {
            ret = r.skip(ftype);
          }
        });
      } else if (10 == fid) {  // INTEGER
        THRIFT_STRUCT_LOOP(r, {
          if (1 == fid && ThriftCompactReader::T_BYTE == ftype) {
            ret = r.read_byte(info.bit_width_);
          } else if (2 == fid && (ThriftCompactReader::T_BOOL_TRUE == ftype
                                  || ThriftCompactReader::T_BOOL_FALSE == ftype)) {
            info.is_signed_ = bval;
          } else {
            ret = r.skip(ftype);
          }
        });
      } else {
        ret = r.skip(ftype);
      }
    }
  });
  return ret;
}

static int parse_schema_element(ThriftCompactReader &r, SchemaElement &e)
{
  int ret = OB_SUCCESS;
  THRIFT_STRUCT_LOOP(r, {
    switch (fid) {
      case 1: ret = read_i32_field(r, ftype, e.type_); break;
      case 2: ret = read_i32_field(r, ftype, e.type_length_); break;
      case 3: ret = read_i32_field(r, ftype, e.repetition_); break;
      case 4:
        ret = ThriftCompactReader::T_BINARY == ftype ? r.read_binary(e.name_) : r.skip(ftype);
        break;
      case 5: ret = read_i32_field(r, ftype, e.num_children_); break;
      case 6: ret = read_i32_field(r, ftype, e.converted_type_); break;
      case 7: ret = read_i32_field(r, ftype, e.scale_); break;
      case 8: ret = read_i32_field(r, ftype, e.precision_); break;
      case 10:
        ret = ThriftCompactReader::T_STRUCT == ftype ? parse_logical_type(r, e.logical_) : r.skip(ftype);
        break;
      default: ret = r.skip(ftype); break;
    }
  });
  return ret;
}

static int parse_column_meta(ThriftCompactReader &r, ColumnChunkMeta &m)
{
  int ret = OB_SUCCESS;
  THRIFT_STRUCT_LOOP(r, {
    switch (fid) {
      case 1: ret = read_i32_field(r, ftype, m.type_); break;
      case 3: {
        if (ThriftCompactReader::T_LIST != ftype) {
          ret = r.skip(ftype);
        } else {
          int8_t elem = 0;
          int64_t size = 0;
          if (OB_SUCC(r.read_list_header(elem, size))) {
            m.path_depth_ = size;
            for (int64_t i = 0; OB_SUCC(ret) && i < size; ++i) {
              ret = r.skip(elem);
            }
          }
        }
        break;
      }
      case 4: ret = read_i32_field(r, ftype, m.codec_); break;
      case 5: ret = read_i64_field(r, ftype, m.num_values_); break;
      case 7: ret = read_i64_field(r, ftype, m.total_compressed_size_); break;
      case 9: ret = read_i64_field(r, ftype, m.data_page_offset_); break;
      case 11: ret = read_i64_field(r, ftype, m.dictionary_page_offset_); break;
      default: ret = r.skip(ftype); break;
    }
  });
  return ret;
}

static int parse_column_chunk(ThriftCompactReader &r, ColumnChunkMeta &m)
{
  int ret = OB_SUCCESS;
  bool has_meta = false;
  THRIFT_STRUCT_LOOP(r, {
    if (1 == fid && ThriftCompactReader::T_BINARY == ftype) {
      ObString file_path;
      if (OB_SUCC(r.read_binary(file_path)) && !file_path.empty()) {
        ret = OB_NOT_SUPPORTED;
        FORWARD_USER_ERROR_MSG(ret, "read_parquet: column chunks in external files are not supported");
      }
    } else if (3 == fid && ThriftCompactReader::T_STRUCT == ftype) {
      has_meta = true;
      ret = parse_column_meta(r, m);
    } else {
      ret = r.skip(ftype);
    }
  });
  if (OB_SUCC(ret) && !has_meta) {
    ret = OB_INVALID_DATA;
    LOG_WARN("column chunk without metadata", K(ret));
  }
  return ret;
}

static int parse_row_group(ThriftCompactReader &r, RowGroupMeta &g)
{
  int ret = OB_SUCCESS;
  THRIFT_STRUCT_LOOP(r, {
    if (1 == fid && ThriftCompactReader::T_LIST == ftype) {
      int8_t elem = 0;
      int64_t size = 0;
      if (OB_FAIL(r.read_list_header(elem, size))) {
      } else if (ThriftCompactReader::T_STRUCT != elem || size > FILE_SQL_MAX_COLUMNS) {
        ret = OB_INVALID_DATA;
        LOG_WARN("invalid column chunk list", K(ret), K(elem), K(size));
      }
      for (int64_t i = 0; OB_SUCC(ret) && i < size; ++i) {
        ColumnChunkMeta m;
        if (OB_FAIL(parse_column_chunk(r, m))) {
        } else if (OB_FAIL(g.columns_.push_back(m))) {
        }
      }
    } else if (3 == fid) {
      ret = read_i64_field(r, ftype, g.num_rows_);
    } else {
      ret = r.skip(ftype);
    }
  });
  return ret;
}

int parse_file_meta(const char *buf, const int64_t len, FileMeta &meta)
{
  int ret = OB_SUCCESS;
  ThriftCompactReader r(buf, len);
  meta.schema_.reuse();
  meta.row_groups_.reuse();
  THRIFT_STRUCT_LOOP(r, {
    if (2 == fid && ThriftCompactReader::T_LIST == ftype) {
      int8_t elem = 0;
      int64_t size = 0;
      if (OB_FAIL(r.read_list_header(elem, size))) {
      } else if (ThriftCompactReader::T_STRUCT != elem || size > FILE_SQL_MAX_COLUMNS + 1) {
        ret = OB_INVALID_DATA;
        LOG_WARN("invalid schema list", K(ret), K(elem), K(size));
      }
      for (int64_t i = 0; OB_SUCC(ret) && i < size; ++i) {
        SchemaElement e;
        if (OB_FAIL(parse_schema_element(r, e))) {
        } else if (OB_FAIL(meta.schema_.push_back(e))) {
        }
      }
    } else if (3 == fid) {
      ret = read_i64_field(r, ftype, meta.num_rows_);
    } else if (4 == fid && ThriftCompactReader::T_LIST == ftype) {
      int8_t elem = 0;
      int64_t size = 0;
      if (OB_FAIL(r.read_list_header(elem, size))) {
      } else if (ThriftCompactReader::T_STRUCT != elem) {
        ret = OB_INVALID_DATA;
      }
      for (int64_t i = 0; OB_SUCC(ret) && i < size; ++i) {
        RowGroupMeta g;
        if (OB_FAIL(parse_row_group(r, g))) {
        } else if (OB_FAIL(meta.row_groups_.push_back(g))) {
        }
      }
    } else {
      ret = r.skip(ftype);
    }
  });
  return ret;
}

int parse_page_header(const char *buf, const int64_t len, PageHeader &h, int64_t &header_len)
{
  int ret = OB_SUCCESS;
  ThriftCompactReader r(buf, len);
  h = PageHeader();
  THRIFT_STRUCT_LOOP(r, {
    switch (fid) {
      case 1: ret = read_i32_field(r, ftype, h.type_); break;
      case 2: ret = read_i32_field(r, ftype, h.uncompressed_size_); break;
      case 3: ret = read_i32_field(r, ftype, h.compressed_size_); break;
      case 5:  // DataPageHeader
        if (ThriftCompactReader::T_STRUCT != ftype) {
          ret = r.skip(ftype);
        } else {
          THRIFT_STRUCT_LOOP(r, {
            switch (fid) {
              case 1: ret = read_i32_field(r, ftype, h.num_values_); break;
              case 2: ret = read_i32_field(r, ftype, h.encoding_); break;
              case 3: ret = read_i32_field(r, ftype, h.def_level_encoding_); break;
              default: ret = r.skip(ftype); break;
            }
          });
        }
        break;
      case 7:  // DictionaryPageHeader
        if (ThriftCompactReader::T_STRUCT != ftype) {
          ret = r.skip(ftype);
        } else {
          THRIFT_STRUCT_LOOP(r, {
            switch (fid) {
              case 1: ret = read_i32_field(r, ftype, h.num_values_); break;
              case 2: ret = read_i32_field(r, ftype, h.encoding_); break;
              default: ret = r.skip(ftype); break;
            }
          });
        }
        break;
      case 8:  // DataPageHeaderV2
        if (ThriftCompactReader::T_STRUCT != ftype) {
          ret = r.skip(ftype);
        } else {
          THRIFT_STRUCT_LOOP(r, {
            switch (fid) {
              case 1: ret = read_i32_field(r, ftype, h.num_values_); break;
              case 2: ret = read_i32_field(r, ftype, h.num_nulls_); break;
              case 4: ret = read_i32_field(r, ftype, h.encoding_); break;
              case 5: ret = read_i32_field(r, ftype, h.def_levels_len_); break;
              case 6: ret = read_i32_field(r, ftype, h.rep_levels_len_); break;
              case 7:
                if (ThriftCompactReader::T_BOOL_TRUE == ftype || ThriftCompactReader::T_BOOL_FALSE == ftype) {
                  h.is_compressed_ = bval;
                } else {
                  ret = r.skip(ftype);
                }
                break;
              default: ret = r.skip(ftype); break;
            }
          });
        }
        break;
      default: ret = r.skip(ftype); break;
    }
  });
  if (OB_SUCC(ret)) {
    header_len = r.get_pos();
  }
  return ret;
}

#undef THRIFT_STRUCT_LOOP

// -------------------------------------------------------------------- snappy

int snappy_uncompressed_length(const char *src, const int64_t src_len, int64_t &len)
{
  int ret = OB_SUCCESS;
  uint64_t v = 0;
  int shift = 0;
  int64_t pos = 0;
  bool done = false;
  while (OB_SUCC(ret) && !done) {
    if (pos >= src_len || shift > 28) {
      ret = OB_INVALID_DATA;
    } else {
      const uint8_t b = static_cast<uint8_t>(src[pos++]);
      v |= static_cast<uint64_t>(b & 0x7F) << shift;
      shift += 7;
      done = 0 == (b & 0x80);
    }
  }
  if (OB_SUCC(ret)) {
    len = static_cast<int64_t>(v);
  }
  return ret;
}

int snappy_decompress(const char *src, const int64_t src_len, char *dst, const int64_t dst_len)
{
  int ret = OB_SUCCESS;
  const uint8_t *ip = reinterpret_cast<const uint8_t *>(src);
  const uint8_t *end = ip + src_len;
  int64_t expect = 0;
  int64_t op = 0;
  // skip the length preamble
  if (OB_FAIL(snappy_uncompressed_length(src, src_len, expect))) {
  } else if (expect != dst_len) {
    ret = OB_INVALID_DATA;
  } else {
    while (ip < end && (*ip & 0x80)) { ++ip; }
    ++ip;
  }
  while (OB_SUCC(ret) && ip < end) {
    const uint8_t tag = *ip++;
    int64_t len = 0;
    int64_t offset = 0;
    switch (tag & 3) {
      case 0: {
        len = tag >> 2;
        if (len >= 60) {
          const int64_t nb = len - 59;
          if (end - ip < nb) {
            ret = OB_INVALID_DATA;
          } else {
            len = 0;
            for (int64_t i = 0; i < nb; ++i) {
              len |= static_cast<int64_t>(ip[i]) << (8 * i);
            }
            ip += nb;
          }
        }
        len += 1;
        if (OB_FAIL(ret)) {
        } else if (end - ip < len || dst_len - op < len) {
          ret = OB_INVALID_DATA;
        } else {
          MEMCPY(dst + op, ip, len);
          ip += len;
          op += len;
        }
        break;
      }
      case 1: {
        if (end - ip < 1) {
          ret = OB_INVALID_DATA;
        } else {
          len = ((tag >> 2) & 7) + 4;
          offset = (static_cast<int64_t>(tag >> 5) << 8) | ip[0];
          ip += 1;
        }
        break;
      }
      case 2: {
        if (end - ip < 2) {
          ret = OB_INVALID_DATA;
        } else {
          len = (tag >> 2) + 1;
          offset = ip[0] | (static_cast<int64_t>(ip[1]) << 8);
          ip += 2;
        }
        break;
      }
      default: {
        if (end - ip < 4) {
          ret = OB_INVALID_DATA;
        } else {
          len = (tag >> 2) + 1;
          offset = ip[0] | (static_cast<int64_t>(ip[1]) << 8) | (static_cast<int64_t>(ip[2]) << 16)
                   | (static_cast<int64_t>(ip[3]) << 24);
          ip += 4;
        }
        break;
      }
    }
    if (OB_SUCC(ret) && 0 != (tag & 3)) {
      if (offset <= 0 || offset > op || dst_len - op < len) {
        ret = OB_INVALID_DATA;
      } else {
        // overlapping copies are legal, copy byte by byte
        for (int64_t i = 0; i < len; ++i) {
          dst[op + i] = dst[op - offset + i];
        }
        op += len;
      }
    }
  }
  if (OB_SUCC(ret) && op != dst_len) {
    ret = OB_INVALID_DATA;
  }
  if (OB_FAIL(ret)) {
    LOG_WARN("invalid snappy data", K(ret), K(src_len), K(dst_len), K(op));
  }
  return ret;
}

// ------------------------------------------------------- RleBitPackedDecoder

int RleBitPackedDecoder::init(const uint8_t *data, const int64_t len, const int32_t bit_width)
{
  int ret = OB_SUCCESS;
  if (bit_width < 0 || bit_width > 32 || len < 0) {
    ret = OB_INVALID_DATA;
    LOG_WARN("invalid rle bit width", K(ret), K(bit_width));
  } else {
    data_ = data;
    len_ = len;
    pos_ = 0;
    bit_width_ = bit_width;
    rle_left_ = 0;
    rle_value_ = 0;
    packed_left_ = 0;
    packed_bit_pos_ = 0;
  }
  return ret;
}

int RleBitPackedDecoder::read_header()
{
  int ret = OB_SUCCESS;
  uint64_t h = 0;
  int shift = 0;
  bool done = false;
  while (OB_SUCC(ret) && !done) {
    if (pos_ >= len_ || shift > 35) {
      ret = OB_INVALID_DATA;
    } else {
      const uint8_t b = data_[pos_++];
      h |= static_cast<uint64_t>(b & 0x7F) << shift;
      shift += 7;
      done = 0 == (b & 0x80);
    }
  }
  if (OB_FAIL(ret)) {
  } else if (h & 1) {
    const int64_t groups = static_cast<int64_t>(h >> 1);
    packed_left_ = groups * 8;
    packed_bit_pos_ = pos_ * 8;
    pos_ = MIN(len_, pos_ + groups * bit_width_);
  } else {
    const int64_t width = (bit_width_ + 7) / 8;
    rle_left_ = static_cast<int64_t>(h >> 1);
    rle_value_ = 0;
    if (pos_ + width > len_) {
      ret = OB_INVALID_DATA;
    } else {
      for (int64_t i = 0; i < width; ++i) {
        rle_value_ |= static_cast<uint32_t>(data_[pos_ + i]) << (8 * i);
      }
      pos_ += width;
    }
  }
  return ret;
}

int RleBitPackedDecoder::next(uint32_t &v)
{
  int ret = OB_SUCCESS;
  if (0 == bit_width_) {
    v = 0;
  } else {
    while (OB_SUCC(ret) && 0 == rle_left_ && 0 == packed_left_) {
      ret = read_header();
    }
    if (OB_FAIL(ret)) {
      LOG_WARN("rle data exhausted", K(ret));
    } else if (rle_left_ > 0) {
      v = rle_value_;
      --rle_left_;
    } else {
      v = 0;
      for (int32_t i = 0; OB_SUCC(ret) && i < bit_width_; ++i) {
        const int64_t bp = packed_bit_pos_ + i;
        if ((bp >> 3) >= len_) {
          ret = OB_INVALID_DATA;
        } else {
          v |= static_cast<uint32_t>((data_[bp >> 3] >> (bp & 7)) & 1) << i;
        }
      }
      packed_bit_pos_ += bit_width_;
      --packed_left_;
    }
  }
  return ret;
}

}  // namespace parquet

using namespace parquet;

// -------------------------------------------------------- ParquetColumnReader

ParquetColumnReader::ParquetColumnReader()
  : fd_(-1), path_(), file_size_(0), type_(-1), type_length_(0), codec_(0), max_def_level_(0),
    next_page_offset_(0), chunk_end_(0), values_left_in_chunk_(0), raw_page_(), page_(),
    values_(nullptr), values_len_(0), values_pos_(0), values_bit_pos_(0), page_values_left_(0),
    page_encoding_(0), has_def_levels_(false), def_decoder_(), value_decoder_(),
    has_dictionary_(false), dict_data_(), dict_(), header_buf_(),
    decompress_allocator_(ObMemAttr("FileSqlPqZ"))
{
}

void ParquetColumnReader::reset()
{
  raw_page_.reset();
  page_.reset();
  dict_data_.reset();
  header_buf_.reset();
  dict_.reset();
  has_dictionary_ = false;
  page_values_left_ = 0;
}

int ParquetColumnReader::fail(const char *msg)
{
  char buf[FILE_SQL_ERROR_MSG_LEN];
  snprintf(buf, sizeof(buf), "corrupted file: %s", msg);
  return report_file_data_error(ObString("read_parquet"), buf);
}

int ParquetColumnReader::init(const int fd, const ObString &path, const int64_t file_size,
                              const ColumnChunkMeta &meta, const SchemaElement &schema,
                              const int16_t max_def_level)
{
  int ret = OB_SUCCESS;
  fd_ = fd;
  path_ = path;
  file_size_ = file_size;
  type_ = meta.type_;
  type_length_ = schema.type_length_;
  codec_ = meta.codec_;
  max_def_level_ = max_def_level;
  values_left_in_chunk_ = meta.num_values_;
  page_values_left_ = 0;
  has_dictionary_ = false;
  dict_.reuse();
  int64_t start = meta.data_page_offset_;
  if (meta.dictionary_page_offset_ > 0 && meta.dictionary_page_offset_ < start) {
    start = meta.dictionary_page_offset_;
  }
  next_page_offset_ = start;
  chunk_end_ = start + meta.total_compressed_size_;
  if (start < 4 || meta.total_compressed_size_ <= 0 || chunk_end_ > file_size_ || chunk_end_ < start
      || meta.num_values_ < 0) {
    ret = fail("invalid column chunk range");
  } else if (PT_FIXED_LEN_BYTE_ARRAY == type_ && (type_length_ <= 0 || type_length_ > 1024 * 1024)) {
    ret = fail("invalid fixed length byte array width");
  } else if (CODEC_UNCOMPRESSED != codec_ && CODEC_SNAPPY != codec_ && CODEC_GZIP != codec_
             && CODEC_ZSTD != codec_) {
    ret = OB_NOT_SUPPORTED;
    FORWARD_USER_ERROR_MSG(ret, "read_parquet: compression codec %d is not supported "
                           "(supported: UNCOMPRESSED, SNAPPY, GZIP, ZSTD)", codec_);
  }
  return ret;
}

int ParquetColumnReader::read_bytes(const int64_t offset, const int64_t len, FileBuffer &buf)
{
  int ret = OB_SUCCESS;
  int64_t read_len = 0;
  if (OB_FAIL(buf.resize(len))) {
  } else if (OB_FAIL(pread_full(fd_, buf.ptr(), len, offset, read_len))) {
  } else if (read_len != len) {
    ret = fail("unexpected end of file");
  }
  return ret;
}

int ParquetColumnReader::decompress(const char *src, const int64_t src_len, const int64_t dst_len,
                                    FileBuffer &dst)
{
  int ret = OB_SUCCESS;
  const int64_t base = dst.length();
  if (dst_len < 0 || dst_len > FILE_SQL_MAX_PARQUET_PAGE_SIZE) {
    ret = fail("invalid page size");
  } else if (OB_FAIL(dst.resize(base + dst_len))) {
  } else if (CODEC_UNCOMPRESSED == codec_) {
    if (src_len != dst_len) {
      ret = fail("page size mismatch");
    } else if (dst_len > 0) {
      MEMCPY(dst.ptr() + base, src, dst_len);
    }
  } else if (CODEC_SNAPPY == codec_) {
    if (OB_FAIL(snappy_decompress(src, src_len, dst.ptr() + base, dst_len))) {
      ret = fail("invalid snappy data");
    }
  } else if (CODEC_GZIP == codec_) {
    z_stream zs;
    MEMSET(&zs, 0, sizeof(zs));
    zs.zalloc = ob_zlib_alloc;
    zs.zfree = ob_zlib_free;
    zs.opaque = static_cast<voidpf>(&decompress_allocator_);
    if (Z_OK != inflateInit2(&zs, 32 + MAX_WBITS)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
      LOG_WARN("inflateInit2 failed", K(ret));
    } else {
      zs.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(src));
      zs.avail_in = static_cast<uInt>(src_len);
      zs.next_out = reinterpret_cast<Bytef *>(dst.ptr() + base);
      zs.avail_out = static_cast<uInt>(dst_len);
      const int zret = inflate(&zs, Z_FINISH);
      if (Z_STREAM_END != zret || static_cast<int64_t>(zs.total_out) != dst_len) {
        ret = fail("invalid gzip data");
      }
      inflateEnd(&zs);
    }
  } else if (CODEC_ZSTD == codec_) {
    zstd_1_3_8::OB_ZSTD_customMem mem = {ob_zstd_malloc, ob_zstd_free, &decompress_allocator_};
    size_t out = 0;
    if (OB_FAIL(zstd_1_3_8::ObZstdWrapper::decompress(mem, src, src_len, dst.ptr() + base, dst_len,
                                                      out))
        || static_cast<int64_t>(out) != dst_len) {
      ret = fail("invalid zstd data");
    }
  }
  return ret;
}

int ParquetColumnReader::read_plain_at(const uint8_t *data, const int64_t len, int64_t &pos,
                                       int64_t &bit_pos, ParquetRawValue &value)
{
  int ret = OB_SUCCESS;
  value.is_null_ = false;
  switch (type_) {
    case PT_BOOLEAN: {
      if ((bit_pos >> 3) >= len) {
        ret = fail("boolean values exhausted");
      } else {
        value.int_ = (data[bit_pos >> 3] >> (bit_pos & 7)) & 1;
        ++bit_pos;
      }
      break;
    }
    case PT_INT32: {
      int32_t v = 0;
      if (pos + 4 > len) {
        ret = fail("int32 values exhausted");
      } else {
        MEMCPY(&v, data + pos, 4);
        pos += 4;
        value.int_ = v;
      }
      break;
    }
    case PT_INT64: {
      int64_t v = 0;
      if (pos + 8 > len) {
        ret = fail("int64 values exhausted");
      } else {
        MEMCPY(&v, data + pos, 8);
        pos += 8;
        value.int_ = v;
      }
      break;
    }
    case PT_INT96: {
      if (pos + 12 > len) {
        ret = fail("int96 values exhausted");
      } else {
        value.str_.assign_ptr(reinterpret_cast<const char *>(data + pos), 12);
        pos += 12;
      }
      break;
    }
    case PT_FLOAT: {
      float v = 0;
      if (pos + 4 > len) {
        ret = fail("float values exhausted");
      } else {
        MEMCPY(&v, data + pos, 4);
        pos += 4;
        value.double_ = v;
      }
      break;
    }
    case PT_DOUBLE: {
      double v = 0;
      if (pos + 8 > len) {
        ret = fail("double values exhausted");
      } else {
        MEMCPY(&v, data + pos, 8);
        pos += 8;
        value.double_ = v;
      }
      break;
    }
    case PT_BYTE_ARRAY: {
      uint32_t n = 0;
      if (pos + 4 > len) {
        ret = fail("byte array values exhausted");
      } else {
        MEMCPY(&n, data + pos, 4);
        pos += 4;
        if (n > static_cast<uint64_t>(len - pos)) {
          ret = fail("byte array value out of bounds");
        } else {
          value.str_.assign_ptr(reinterpret_cast<const char *>(data + pos), static_cast<int32_t>(n));
          pos += n;
        }
      }
      break;
    }
    case PT_FIXED_LEN_BYTE_ARRAY: {
      if (pos + type_length_ > len) {
        ret = fail("fixed byte array values exhausted");
      } else {
        value.str_.assign_ptr(reinterpret_cast<const char *>(data + pos), type_length_);
        pos += type_length_;
      }
      break;
    }
    default:
      ret = fail("unknown physical type");
  }
  return ret;
}

int ParquetColumnReader::load_dictionary(const PageHeader &header, const char *data, const int64_t len)
{
  int ret = OB_SUCCESS;
  dict_.reuse();
  dict_data_.reuse();
  if (has_dictionary_) {
    ret = fail("duplicate dictionary page");
  } else if (ENC_PLAIN != header.encoding_ && ENC_PLAIN_DICTIONARY != header.encoding_) {
    ret = fail("unsupported dictionary encoding");
  } else if (header.num_values_ < 0 || header.num_values_ > len * 8 + 1) {
    ret = fail("invalid dictionary size");
  } else if (OB_FAIL(decompress(data, len, header.uncompressed_size_, dict_data_))) {
  } else {
    int64_t pos = 0;
    int64_t bit_pos = 0;
    const uint8_t *d = reinterpret_cast<const uint8_t *>(dict_data_.ptr());
    for (int64_t i = 0; OB_SUCC(ret) && i < header.num_values_; ++i) {
      ParquetRawValue v;
      if (OB_FAIL(read_plain_at(d, dict_data_.length(), pos, bit_pos, v))) {
      } else if (OB_FAIL(dict_.push_back(v))) {
      }
    }
    has_dictionary_ = OB_SUCC(ret);
  }
  return ret;
}

int ParquetColumnReader::load_next_page()
{
  int ret = OB_SUCCESS;
  bool loaded = false;
  while (OB_SUCC(ret) && !loaded) {
    PageHeader h;
    int64_t header_len = 0;
    if (next_page_offset_ >= chunk_end_) {
      ret = fail("column chunk has fewer values than rows");
      break;
    }
    // page headers are small but may carry statistics; grow the window on demand
    int64_t window = MIN(64 * 1024, chunk_end_ - next_page_offset_);
    bool parsed = false;
    while (OB_SUCC(ret) && !parsed) {
      if (OB_FAIL(read_bytes(next_page_offset_, window, header_buf_))) {
      } else if (OB_SUCCESS == parse_page_header(header_buf_.ptr(), window, h, header_len)) {
        parsed = true;
      } else if (window >= chunk_end_ - next_page_offset_ || window >= 16 * 1024 * 1024) {
        ret = fail("invalid page header");
      } else {
        window = MIN(window * 4, chunk_end_ - next_page_offset_);
      }
    }
    if (OB_FAIL(ret)) {
    } else if (h.compressed_size_ < 0 || h.uncompressed_size_ < 0
               || h.compressed_size_ > FILE_SQL_MAX_PARQUET_PAGE_SIZE
               || h.uncompressed_size_ > FILE_SQL_MAX_PARQUET_PAGE_SIZE
               || next_page_offset_ + header_len + h.compressed_size_ > chunk_end_) {
      ret = fail("invalid page size");
    } else {
      const int64_t data_offset = next_page_offset_ + header_len;
      next_page_offset_ = data_offset + h.compressed_size_;
      if (PAGE_INDEX == h.type_) {
        // not needed for a full scan
      } else if (PAGE_DICTIONARY == h.type_) {
        if (OB_FAIL(read_bytes(data_offset, h.compressed_size_, raw_page_))) {
        } else if (OB_FAIL(load_dictionary(h, raw_page_.ptr(), raw_page_.length()))) {
        }
      } else if (PAGE_DATA == h.type_ || PAGE_DATA_V2 == h.type_) {
        if (h.num_values_ < 0 || h.num_values_ > values_left_in_chunk_) {
          ret = fail("invalid number of values in page");
        } else if (OB_FAIL(read_bytes(data_offset, h.compressed_size_, raw_page_))) {
        } else {
          page_.reuse();
          int64_t levels_len = 0;
          if (PAGE_DATA == h.type_) {
            ret = decompress(raw_page_.ptr(), raw_page_.length(), h.uncompressed_size_, page_);
          } else {
            levels_len = static_cast<int64_t>(h.def_levels_len_) + h.rep_levels_len_;
            if (h.def_levels_len_ < 0 || h.rep_levels_len_ < 0 || levels_len > raw_page_.length()
                || levels_len > h.uncompressed_size_) {
              ret = fail("invalid level lengths");
            } else if (OB_FAIL(page_.append(raw_page_.ptr(), levels_len))) {
            } else if (h.is_compressed_) {
              ret = decompress(raw_page_.ptr() + levels_len, raw_page_.length() - levels_len,
                               h.uncompressed_size_ - levels_len, page_);
            } else if (raw_page_.length() - levels_len != h.uncompressed_size_ - levels_len) {
              ret = fail("page size mismatch");
            } else {
              ret = page_.append(raw_page_.ptr() + levels_len, raw_page_.length() - levels_len);
            }
          }
          const uint8_t *d = reinterpret_cast<const uint8_t *>(page_.ptr());
          const int64_t dlen = page_.length();
          int64_t pos = 0;
          has_def_levels_ = max_def_level_ > 0;
          if (OB_FAIL(ret)) {
          } else if (PAGE_DATA == h.type_) {
            if (has_def_levels_) {
              uint32_t n = 0;
              if (ENC_RLE != h.def_level_encoding_) {
                ret = OB_NOT_SUPPORTED;
                FORWARD_USER_ERROR_MSG(ret, "read_parquet: definition level encoding %d is not supported",
                                       h.def_level_encoding_);
              } else if (pos + 4 > dlen) {
                ret = fail("truncated definition levels");
              } else {
                MEMCPY(&n, d + pos, 4);
                pos += 4;
                if (n > static_cast<uint64_t>(dlen - pos)) {
                  ret = fail("truncated definition levels");
                } else if (OB_FAIL(def_decoder_.init(d + pos, n, 1))) {
                } else {
                  pos += n;
                }
              }
            }
          } else {
            pos = h.rep_levels_len_;
            if (has_def_levels_ && OB_FAIL(def_decoder_.init(d + pos, h.def_levels_len_, 1))) {
            } else {
              pos += h.def_levels_len_;
            }
          }
          if (OB_SUCC(ret)) {
            values_ = d + pos;
            values_len_ = dlen - pos;
            values_pos_ = 0;
            values_bit_pos_ = 0;
            page_encoding_ = h.encoding_;
            if (ENC_PLAIN == page_encoding_) {
            } else if (ENC_PLAIN_DICTIONARY == page_encoding_ || ENC_RLE_DICTIONARY == page_encoding_) {
              if (!has_dictionary_) {
                ret = fail("dictionary page is missing");
              } else if (values_len_ < 1) {
                ret = fail("truncated dictionary indexes");
              } else if (OB_FAIL(value_decoder_.init(values_ + 1, values_len_ - 1, values_[0]))) {
                ret = fail("invalid dictionary index bit width");
              }
            } else if (ENC_RLE == page_encoding_ && PT_BOOLEAN == type_) {
              uint32_t n = 0;
              if (values_len_ < 4) {
                ret = fail("truncated boolean values");
              } else {
                MEMCPY(&n, values_, 4);
                if (n > static_cast<uint64_t>(values_len_ - 4)) {
                  ret = fail("truncated boolean values");
                } else {
                  ret = value_decoder_.init(values_ + 4, n, 1);
                }
              }
            } else {
              ret = OB_NOT_SUPPORTED;
              FORWARD_USER_ERROR_MSG(ret, "read_parquet: value encoding %d is not supported "
                                     "(supported: PLAIN, PLAIN_DICTIONARY, RLE_DICTIONARY)",
                                     page_encoding_);
            }
          }
          if (OB_SUCC(ret)) {
            page_values_left_ = h.num_values_;
            loaded = h.num_values_ > 0;
          }
        }
      } else {
        ret = fail("unknown page type");
      }
    }
  }
  return ret;
}

int ParquetColumnReader::decode_plain(ParquetRawValue &value)
{
  return read_plain_at(values_, values_len_, values_pos_, values_bit_pos_, value);
}

int ParquetColumnReader::next(ParquetRawValue &value)
{
  int ret = OB_SUCCESS;
  if (0 == page_values_left_ && OB_FAIL(load_next_page())) {
    LOG_WARN("failed to load page", K(ret));
  } else {
    uint32_t def = static_cast<uint32_t>(max_def_level_);
    if (has_def_levels_ && OB_FAIL(def_decoder_.next(def))) {
      ret = fail("invalid definition levels");
    } else if (def < static_cast<uint32_t>(max_def_level_)) {
      value.is_null_ = true;
    } else if (ENC_PLAIN == page_encoding_) {
      ret = decode_plain(value);
    } else if (ENC_RLE == page_encoding_) {
      uint32_t b = 0;
      if (OB_FAIL(value_decoder_.next(b))) {
        ret = fail("invalid boolean values");
      } else {
        value.is_null_ = false;
        value.int_ = b & 1;
      }
    } else {
      uint32_t idx = 0;
      if (OB_FAIL(value_decoder_.next(idx))) {
        ret = fail("invalid dictionary indexes");
      } else if (idx >= static_cast<uint32_t>(dict_.count())) {
        ret = fail("dictionary index out of range");
      } else {
        value = dict_.at(idx);
      }
    }
    if (OB_SUCC(ret)) {
      --page_values_left_;
      --values_left_in_chunk_;
    }
  }
  return ret;
}

// ------------------------------------------------------------ ParquetScanner

int ParquetScanner::load_meta(const int fd, const ObString &path, const int64_t file_size,
                              FileBuffer &meta_buf, FileMeta &meta)
{
  int ret = OB_SUCCESS;
  char tail[8];
  char head[4];
  int64_t read_len = 0;
  uint32_t meta_len = 0;
  UNUSED(path);
  if (file_size < 12) {
    ret = report_file_data_error(ObString("read_parquet"), "not a parquet file (too small)");
  } else if (OB_FAIL(pread_full(fd, head, 4, 0, read_len)) || 4 != read_len) {
    ret = OB_SUCC(ret) ? OB_IO_ERROR : ret;
  } else if (OB_FAIL(pread_full(fd, tail, 8, file_size - 8, read_len)) || 8 != read_len) {
    ret = OB_SUCC(ret) ? OB_IO_ERROR : ret;
  } else if (0 != MEMCMP(head, "PAR1", 4) || 0 != MEMCMP(tail + 4, "PAR1", 4)) {
    ret = report_file_data_error(ObString("read_parquet"), "not a parquet file (bad magic)");
  } else {
    MEMCPY(&meta_len, tail, 4);
    if (meta_len == 0 || meta_len > static_cast<uint64_t>(file_size - 12)
        || meta_len > FILE_SQL_MAX_PARQUET_FOOTER_SIZE) {
      ret = report_file_data_error(ObString("read_parquet"), "corrupted file: invalid footer length");
    } else if (OB_FAIL(meta_buf.resize(meta_len))) {
    } else if (OB_FAIL(pread_full(fd, meta_buf.ptr(), meta_len, file_size - 8 - meta_len, read_len))
               || read_len != meta_len) {
      ret = OB_SUCC(ret) ? OB_IO_ERROR : ret;
    } else if (OB_FAIL(parse_file_meta(meta_buf.ptr(), meta_len, meta))) {
      if (OB_NOT_SUPPORTED != ret) {
        ret = report_file_data_error(ObString("read_parquet"), "corrupted file: invalid footer");
      }
    }
  }
  // flat schema validation
  if (OB_SUCC(ret)) {
    int64_t total_rows = 0;
    const int64_t leaf_cnt = meta.schema_.count() - 1;
    if (meta.schema_.count() < 2 || meta.schema_.at(0).num_children_ != leaf_cnt) {
      ret = OB_NOT_SUPPORTED;
      FORWARD_USER_ERROR_MSG(ret, "read_parquet: only flat schemas are supported");
    }
    for (int64_t i = 1; OB_SUCC(ret) && i < meta.schema_.count(); ++i) {
      const SchemaElement &e = meta.schema_.at(i);
      if (e.num_children_ > 0 || 2 == e.repetition_ || e.type_ < PT_BOOLEAN
          || e.type_ > PT_FIXED_LEN_BYTE_ARRAY) {
        ret = OB_NOT_SUPPORTED;
        FORWARD_USER_ERROR_MSG(ret, "read_parquet: nested or repeated column '%.*s' is not supported",
                               e.name_.length(), e.name_.ptr());
      }
    }
    for (int64_t g = 0; OB_SUCC(ret) && g < meta.row_groups_.count(); ++g) {
      const RowGroupMeta &rg = meta.row_groups_.at(g);
      if (rg.columns_.count() != leaf_cnt || rg.num_rows_ < 0) {
        ret = report_file_data_error(ObString("read_parquet"), "corrupted file: row group mismatch");
      }
      for (int64_t c = 0; OB_SUCC(ret) && c < rg.columns_.count(); ++c) {
        const ColumnChunkMeta &cm = rg.columns_.at(c);
        if (cm.type_ != meta.schema_.at(c + 1).type_ || cm.path_depth_ > 1
            || cm.num_values_ < rg.num_rows_) {
          ret = report_file_data_error(ObString("read_parquet"), "corrupted file: column chunk mismatch");
        }
      }
      total_rows += rg.num_rows_;
    }
    if (OB_SUCC(ret) && total_rows != meta.num_rows_) {
      ret = report_file_data_error(ObString("read_parquet"), "corrupted file: row count mismatch");
    }
  }
  return ret;
}

static int map_parquet_column(const SchemaElement &e, FileColumnDesc &col)
{
  int ret = OB_SUCCESS;
  const int16_t lt = e.logical_.kind_;
  const int32_t ct = e.converted_type_;
  const bool is_decimal = 5 == lt || 5 == ct;
  const int32_t scale = 5 == lt ? e.logical_.scale_ : e.scale_;
  const bool is_date = 6 == lt || 6 == ct;
  const bool is_ts = 8 == lt || 9 == ct || 10 == ct;
  const int16_t unit = 8 == lt ? e.logical_.unit_ : (9 == ct ? 1 : 2);
  const bool is_unsigned = (10 == lt && !e.logical_.is_signed_) || (ct >= 11 && ct <= 14);
  col.nullable_ = 1 == e.repetition_;
  col.type_length_ = e.type_length_;
  col.convert_ = FileValueConvert::NONE;
  if (is_decimal && (scale < 0 || scale > 38)) {
    ret = OB_NOT_SUPPORTED;
    FORWARD_USER_ERROR_MSG(ret, "read_parquet: decimal scale %d is not supported", scale);
  } else {
    switch (e.type_) {
      case PT_BOOLEAN:
        col.source_ = FileValueSource::PARQUET_BOOLEAN;
        col.type_ = FileColumnType::BOOLEAN;
        break;
      case PT_INT32:
      case PT_INT64:
        col.source_ = PT_INT32 == e.type_ ? FileValueSource::PARQUET_INT32 : FileValueSource::PARQUET_INT64;
        if (is_decimal) {
          col.type_ = FileColumnType::DOUBLE;
          col.convert_ = FileValueConvert::DECIMAL;
          col.param_ = scale;
        } else if (is_date && PT_INT32 == e.type_) {
          col.type_ = FileColumnType::DATE;
          col.convert_ = FileValueConvert::DATE_DAYS;
        } else if (is_ts && PT_INT64 == e.type_) {
          col.type_ = FileColumnType::DATETIME;
          col.convert_ = 1 == unit ? FileValueConvert::TIMESTAMP_MILLIS
                         : (3 == unit ? FileValueConvert::TIMESTAMP_NANOS : FileValueConvert::TIMESTAMP_MICROS);
          col.scale_ = 1 == unit ? 3 : 6;
        } else if (is_unsigned) {
          col.type_ = PT_INT32 == e.type_ ? FileColumnType::BIGINT : FileColumnType::DOUBLE;
          col.convert_ = FileValueConvert::UNSIGNED;
        } else {
          col.type_ = FileColumnType::BIGINT;
        }
        break;
      case PT_INT96:
        col.source_ = FileValueSource::PARQUET_INT96;
        col.type_ = FileColumnType::DATETIME;
        col.convert_ = FileValueConvert::INT96_TIMESTAMP;
        col.scale_ = 6;
        break;
      case PT_FLOAT:
      case PT_DOUBLE:
        col.source_ = PT_FLOAT == e.type_ ? FileValueSource::PARQUET_FLOAT : FileValueSource::PARQUET_DOUBLE;
        col.type_ = FileColumnType::DOUBLE;
        break;
      case PT_BYTE_ARRAY:
      case PT_FIXED_LEN_BYTE_ARRAY:
        col.source_ = PT_BYTE_ARRAY == e.type_ ? FileValueSource::PARQUET_BYTE_ARRAY
                                                : FileValueSource::PARQUET_FIXED_BYTE_ARRAY;
        if (is_decimal) {
          col.type_ = FileColumnType::DOUBLE;
          col.convert_ = FileValueConvert::DECIMAL;
          col.param_ = scale;
        } else {
          col.type_ = FileColumnType::VARCHAR;
          col.max_length_ = FILE_SQL_MAX_VARCHAR_LENGTH;
        }
        break;
      default:
        ret = OB_NOT_SUPPORTED;
    }
  }
  return ret;
}

int infer_parquet_schema(ObIAllocator &allocator, FileScanDesc &desc)
{
  int ret = OB_SUCCESS;
  int fd = -1;
  FileFingerprint fp;
  FileBuffer meta_buf;
  FileMeta meta;
  desc.columns_.reuse();
  if (OB_FAIL(open_input_file(desc.path_, nullptr, fd, fp))) {
  } else if (OB_FAIL(ParquetScanner::load_meta(fd, desc.path_, fp.size_, meta_buf, meta))) {
  } else {
    desc.fingerprint_ = fp;
    desc.row_count_ = meta.num_rows_;
    for (int64_t i = 1; OB_SUCC(ret) && i < meta.schema_.count(); ++i) {
      FileColumnDesc col;
      const SchemaElement &e = meta.schema_.at(i);
      col.source_idx_ = static_cast<int32_t>(i - 1);
      if (!TextValueParser::is_valid_utf8(e.name_.ptr(), e.name_.length())) {
        ret = report_file_data_error(desc.func_name_, "invalid UTF-8 column name");
      } else if (OB_FAIL(make_unique_column_name(allocator, e.name_, i - 1, desc.columns_, col.name_))) {
      } else if (OB_FAIL(map_parquet_column(e, col))) {
      } else if (OB_FAIL(desc.columns_.push_back(col))) {
      }
    }
  }
  if (fd >= 0) {
    ::close(fd);
  }
  return ret;
}

int ParquetScanner::open(const FileScanDesc &desc, const ObIArray<bool> &projected)
{
  int ret = OB_SUCCESS;
  FileFingerprint fp;
  close();
  desc_ = &desc;
  if (OB_FAIL(projected_.assign(projected))) {
  } else if (OB_FAIL(open_input_file(desc.path_, &desc.fingerprint_, fd_, fp))) {
  } else if (OB_FAIL(load_meta(fd_, desc.path_, fp.size_, meta_buf_, meta_))) {
  } else if (meta_.schema_.count() - 1 != desc.columns_.count()) {
    ret = report_file_data_error(desc.func_name_, "file schema has changed");
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < desc.columns_.count(); ++i) {
    ParquetColumnReader *reader = nullptr;
    if (projected_.at(i)) {
      void *buf = ob_malloc(sizeof(ParquetColumnReader), ObMemAttr("FileSqlPq"));
      if (OB_ISNULL(buf)) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else {
        reader = new (buf) ParquetColumnReader();
      }
    }
    if (OB_SUCC(ret) && OB_FAIL(readers_.push_back(reader))) {
      if (OB_NOT_NULL(reader)) {
        reader->~ParquetColumnReader();
        ob_free(reader);
      }
    }
  }
  row_group_idx_ = -1;
  rows_left_in_group_ = 0;
  return ret;
}

int ParquetScanner::open_row_group(const int64_t idx)
{
  int ret = OB_SUCCESS;
  const RowGroupMeta &rg = meta_.row_groups_.at(idx);
  for (int64_t i = 0; OB_SUCC(ret) && i < readers_.count(); ++i) {
    if (OB_NOT_NULL(readers_.at(i))) {
      const SchemaElement &e = meta_.schema_.at(desc_->columns_.at(i).source_idx_ + 1);
      const int16_t max_def = 1 == e.repetition_ ? 1 : 0;
      ret = readers_.at(i)->init(fd_, desc_->path_, desc_->fingerprint_.size_,
                                 rg.columns_.at(desc_->columns_.at(i).source_idx_), e, max_def);
    }
  }
  if (OB_SUCC(ret)) {
    row_group_idx_ = idx;
    rows_left_in_group_ = rg.num_rows_;
  }
  return ret;
}

int ParquetScanner::rescan()
{
  int ret = OB_SUCCESS;
  if (fd_ < 0) {
    ret = OB_NOT_INIT;
  } else if (OB_FAIL(check_file_unchanged(fd_, desc_->path_, desc_->fingerprint_))) {
  } else {
    row_group_idx_ = -1;
    rows_left_in_group_ = 0;
  }
  return ret;
}

static int decimal_to_double(const char *bytes, const int64_t len, const int32_t scale, double &v)
{
  int ret = OB_SUCCESS;
  if (len <= 0 || len > 32) {
    ret = OB_NOT_SUPPORTED;
  } else {
    // big endian two's complement
    long double acc = 0;
    const bool neg = (static_cast<uint8_t>(bytes[0]) & 0x80) != 0;
    for (int64_t i = 0; i < len; ++i) {
      uint8_t b = static_cast<uint8_t>(bytes[i]);
      acc = acc * 256 + (neg ? static_cast<uint8_t>(~b) : b);
    }
    if (neg) {
      acc = -(acc + 1);
    }
    v = static_cast<double>(acc / powl(10.0L, scale));
  }
  return ret;
}

static int64_t floor_div(const int64_t a, const int64_t b)
{
  int64_t q = a / b;
  if ((a % b != 0) && ((a < 0) != (b < 0))) {
    --q;
  }
  return q;
}

int ParquetScanner::convert_value(const FileColumnDesc &col, const ParquetRawValue &raw, FileValue &value)
{
  int ret = OB_SUCCESS;
  if (raw.is_null_) {
    value.set_null();
  } else {
    switch (col.source_) {
      case FileValueSource::PARQUET_BOOLEAN:
        value.set_int(raw.int_ ? 1 : 0);
        break;
      case FileValueSource::PARQUET_INT32:
      case FileValueSource::PARQUET_INT64: {
        switch (col.convert_) {
          case FileValueConvert::DECIMAL:
            value.set_double(static_cast<double>(raw.int_) / pow(10.0, col.param_));
            break;
          case FileValueConvert::UNSIGNED:
            if (FileValueSource::PARQUET_INT32 == col.source_) {
              value.set_int(static_cast<int64_t>(static_cast<uint32_t>(raw.int_)));
            } else {
              value.set_double(static_cast<double>(static_cast<uint64_t>(raw.int_)));
            }
            break;
          case FileValueConvert::TIMESTAMP_MILLIS:
            value.set_int(raw.int_ * 1000);
            break;
          case FileValueConvert::TIMESTAMP_NANOS:
            value.set_int(floor_div(raw.int_, 1000));
            break;
          default:
            value.set_int(raw.int_);
            break;
        }
        break;
      }
      case FileValueSource::PARQUET_FLOAT:
      case FileValueSource::PARQUET_DOUBLE:
        value.set_double(raw.double_);
        break;
      case FileValueSource::PARQUET_INT96: {
        int64_t nanos = 0;
        int32_t julian = 0;
        MEMCPY(&nanos, raw.str_.ptr(), 8);
        MEMCPY(&julian, raw.str_.ptr() + 8, 4);
        value.set_int((static_cast<int64_t>(julian) - 2440588LL) * USECS_PER_DAY + floor_div(nanos, 1000));
        break;
      }
      case FileValueSource::PARQUET_BYTE_ARRAY:
      case FileValueSource::PARQUET_FIXED_BYTE_ARRAY: {
        if (FileValueConvert::DECIMAL == col.convert_) {
          double d = 0;
          if (OB_FAIL(decimal_to_double(raw.str_.ptr(), raw.str_.length(), col.param_, d))) {
            ret = report_file_data_error(desc_->func_name_, "decimal value is too wide");
          } else {
            value.set_double(d);
          }
        } else if (!TextValueParser::is_valid_utf8(raw.str_.ptr(), raw.str_.length())) {
          char msg[FILE_SQL_ERROR_MSG_LEN];
          snprintf(msg, sizeof(msg), "column '%.*s' contains invalid UTF-8 (binary data is not supported)",
                   col.name_.length(), col.name_.ptr());
          ret = report_file_data_error(desc_->func_name_, msg);
        } else {
          value.set_string(raw.str_);
        }
        break;
      }
      default:
        ret = OB_ERR_UNEXPECTED;
    }
  }
  return ret;
}

int ParquetScanner::get_next_row(ObIArray<FileValue> &row)
{
  int ret = OB_SUCCESS;
  while (OB_SUCC(ret) && 0 == rows_left_in_group_) {
    if (row_group_idx_ + 1 >= meta_.row_groups_.count()) {
      ret = check_file_unchanged(fd_, desc_->path_, desc_->fingerprint_);
      if (OB_SUCC(ret)) {
        ret = OB_ITER_END;
      }
    } else {
      ret = open_row_group(row_group_idx_ + 1);
    }
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < readers_.count(); ++i) {
    FileValue &v = row.at(i);
    v.set_null();
    if (OB_NOT_NULL(readers_.at(i))) {
      ParquetRawValue raw;
      if (OB_FAIL(readers_.at(i)->next(raw))) {
      } else if (OB_FAIL(convert_value(desc_->columns_.at(i), raw, v))) {
      }
    }
  }
  if (OB_SUCC(ret)) {
    --rows_left_in_group_;
  }
  return ret;
}

void ParquetScanner::reset_readers()
{
  for (int64_t i = 0; i < readers_.count(); ++i) {
    if (OB_NOT_NULL(readers_.at(i))) {
      readers_.at(i)->~ParquetColumnReader();
      ob_free(readers_.at(i));
    }
  }
  readers_.reset();
}

void ParquetScanner::close()
{
  reset_readers();
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
  meta_buf_.reset();
  meta_.schema_.reset();
  meta_.row_groups_.reset();
  row_group_idx_ = -1;
  rows_left_in_group_ = 0;
}

}  // namespace file_sql
}  // namespace sql
}  // namespace oceanbase
