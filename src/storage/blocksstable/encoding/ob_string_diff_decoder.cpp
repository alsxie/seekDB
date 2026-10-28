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

#define USING_LOG_PREFIX STORAGE

#include "ob_string_diff_decoder.h"

#include "ob_raw_decoder.h"

namespace oceanbase
{
namespace blocksstable
{
using namespace common;
const ObColumnHeader::Type ObStringDiffDecoder::type_;

namespace
{
// Copies n (<= 32) bytes with fixed size moves instead of a libc call per segment; longer runs
// use MEMCPY.  The head and tail moves may overlap, every byte read or written stays inside
// [src, src + n) and [dst, dst + n).  作者：FDE WorkMate
OB_INLINE void copy_short_bytes(char *dst, const char *src, const int64_t n)
{
  if (n >= 16) {
    if (n <= 32) {
      char head[16];
      char tail[16];
      MEMCPY(head, src, 16);
      MEMCPY(tail, src + n - 16, 16);
      MEMCPY(dst, head, 16);
      MEMCPY(dst + n - 16, tail, 16);
    } else {
      MEMCPY(dst, src, n);
    }
  } else if (n >= 8) {
    uint64_t head = 0;
    uint64_t tail = 0;
    MEMCPY(&head, src, 8);
    MEMCPY(&tail, src + n - 8, 8);
    MEMCPY(dst, &head, 8);
    MEMCPY(dst + n - 8, &tail, 8);
  } else if (n >= 4) {
    uint32_t head = 0;
    uint32_t tail = 0;
    MEMCPY(&head, src, 4);
    MEMCPY(&tail, src + n - 4, 4);
    MEMCPY(dst, &head, 4);
    MEMCPY(dst + n - 4, &tail, 4);
  } else if (n > 0) {
    dst[0] = src[0];
    dst[n >> 1] = src[n >> 1];
    dst[n - 1] = src[n - 1];
  }
}

// Fixed length cells without hex packing.  The common bytes are laid out once as a template,
// then every row gets one template copy plus its diff segments, giving the same bytes as the
// two descriptor walks of the generic path (common part, then diff part) without a libc memcpy
// per descriptor and row.  Returns false, touching nothing, when the layout is outside the
// limits below so the caller runs the generic path.  作者：FDE WorkMate
bool fill_fixed_diff_rows(
    const ObStringDiffHeader &header,
    const bool has_extend_value,
    const unsigned char *cell_base,
    const int32_t *row_ids,
    const int64_t row_cap,
    char *buf,
    const int64_t buf_size,
    common::ObDatum *datums)
{
  static const int64_t MAX_TEMPLATE_SIZE = 32;
  static const int64_t MAX_DIFF_SEGMENTS = 8;
  const int64_t string_size = header.string_size_;
  int64_t seg_full_pos[MAX_DIFF_SEGMENTS];
  int64_t seg_cell_pos[MAX_DIFF_SEGMENTS];
  int64_t seg_len[MAX_DIFF_SEGMENTS];
  int64_t seg_cnt = 0;
  int64_t full_pos = 0;
  int64_t cell_pos = 0;
  bool fits = string_size <= MAX_TEMPLATE_SIZE;
  for (int64_t i = 0; fits && i < header.diff_desc_cnt_; ++i) {
    const int64_t cnt = header.diff_descs_[i].count_;
    if (0 != header.diff_descs_[i].diff_) {
      if (seg_cnt >= MAX_DIFF_SEGMENTS) {
        fits = false;
      } else {
        seg_full_pos[seg_cnt] = full_pos;
        seg_cell_pos[seg_cnt] = cell_pos;
        seg_len[seg_cnt] = cnt;
        ++seg_cnt;
        cell_pos += cnt;
      }
    }
    full_pos += cnt;
  }
  // the descriptors must tile the whole string so the template copy writes only defined bytes
  fits = fits && full_pos == string_size;
  if (fits) {
    char tmpl[MAX_TEMPLATE_SIZE];
    header.copy_string(ObStringDiffHeader::LeftToRight(), std::logical_not<uint8_t>(),
        header.common_data(), tmpl);
    const int64_t cell_len = header.length_;
    for (int64_t i = 0; i < row_cap; ++i) {
      if (has_extend_value && datums[i].is_null()) {
        // Skip
      } else {
        char *str_ptr = buf + i * buf_size;
        const char *cell = reinterpret_cast<const char *>(cell_base) + row_ids[i] * cell_len;
        copy_short_bytes(str_ptr, tmpl, string_size);
        for (int64_t s = 0; s < seg_cnt; ++s) {
          copy_short_bytes(str_ptr + seg_full_pos[s], cell + seg_cell_pos[s], seg_len[s]);
        }
        datums[i].pack_ = static_cast<uint32_t>(string_size);
        datums[i].ptr_ = str_ptr;
      }
    }
  }
  return fits;
}
} // namespace

ObStringDiffDecoder::ObStringDiffDecoder() : header_(NULL)
{
}

ObStringDiffDecoder::~ObStringDiffDecoder()
{
}

int ObStringDiffDecoder::decode(const ObColumnDecoderCtx &ctx, common::ObDatum &datum, const int64_t row_id,
    const ObBitStream &bs, const char *data, const int64_t len) const
{
  int ret = OB_SUCCESS;
  uint64_t val = STORED_NOT_EXT;
  ObBitStream fix_bs;
  const char *col_data = reinterpret_cast<const char *>(header_) + ctx.col_header_->length_;
  int64_t data_offset = 0;

  if (!is_inited()) {
    ret = OB_NOT_INIT;
  } else if (OB_UNLIKELY(nullptr == data || len < 0)) {
    ret = OB_INVALID_ARGUMENT;
  } else {

    // read extend value bit
    if (ctx.has_extend_value()) {
      if (ctx.is_fix_length()) {
        data_offset = ctx.micro_block_header_->row_count_ * ctx.micro_block_header_->extend_value_bit_;
        data_offset = (data_offset + CHAR_BIT - 1) / CHAR_BIT;
        if (OB_FAIL(ObBitStream::get(reinterpret_cast<const unsigned char *>(col_data),
            row_id * ctx.micro_block_header_->extend_value_bit_,
            ctx.micro_block_header_->extend_value_bit_, val))) {
        }
      } else {
        if (OB_FAIL(bs.get(ctx.col_header_->extend_value_index_,
                ctx.micro_block_header_->extend_value_bit_, val))) {
        }
      }
    }
  }

  if (OB_FAIL(ret)) {
  } else if (STORED_NOT_EXT != val) {
    set_stored_ext_value(datum, static_cast<ObStoredExtValue>(val));
  } else {
    const char *cell_data = NULL;
    int64_t cell_len = 0;
    // get cell data offset and length
    if (ctx.is_fix_length()) {
      cell_data = col_data + data_offset + row_id * header_->length_;
    } else {
      if (OB_FAIL(ObRawDecoder::locate_cell_data(cell_data, cell_len, data, len,
              *ctx.micro_block_header_, *ctx.col_header_, *header_))) {
      }
    }

    char *buf = NULL;
    if (OB_SUCC(ret)) {
      const static uint16_t min_buf_size = 128;
      const int64_t buf_size = std::max(header_->string_size_, min_buf_size);
      if (OB_ISNULL(buf = static_cast<char *>(ctx.allocator_->alloc(buf_size)))) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else {
        header_->copy_string(ObStringDiffHeader::LeftToRight(), std::logical_not<uint8_t>(),
            header_->common_data(), buf);
      }
    }

    if (OB_SUCC(ret)) {
      //int64_t cell_len = header_->length_;
      if (header_->is_hex_packing()) {
        ObHexStringUnpacker unpacker(header_->hex_char_array(),
            reinterpret_cast<const unsigned char *>(cell_data));
        header_->copy_hex_string(unpacker,
            static_cast<void (ObHexStringUnpacker::*)(unsigned char &)>(&ObHexStringUnpacker::unpack),
            reinterpret_cast<unsigned char *>(buf));
      } else {
        header_->copy_string(ObStringDiffHeader::LeftToRight(),
            ObStringDiffHeader::LogicTrue<uint8_t>(),
            cell_data, buf);
      }
      datum.pack_ = header_->string_size_;
      datum.ptr_ = buf;
    }
  }

  return ret;
}

int ObStringDiffDecoder::update_pointer(const char *old_block, const char *cur_block)
{
  int ret = OB_SUCCESS;
  if (!is_inited()) {
    ret = OB_NOT_INIT;
  } else if (OB_ISNULL(old_block) || OB_ISNULL(cur_block)) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    ObIColumnDecoder::update_pointer(header_, old_block, cur_block);
  }
  return ret;
}

// Internal call, not check parameters for performance
int ObStringDiffDecoder::batch_decode(
    const ObColumnDecoderCtx &ctx,
    const ObIRowIndex* row_index,
    const int32_t *row_ids,
    const char **cell_datas,
    const int64_t row_cap,
    common::ObDatum *datums) const
{
  UNUSED(cell_datas);
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!is_inited())) {
    ret = OB_NOT_INIT;
  } else {
    const unsigned char *col_data = reinterpret_cast<const unsigned char *>(header_)
        + ctx.col_header_->length_;
    int64_t data_offset = 0;
    if (ctx.has_extend_value()) {
      if (ctx.is_fix_length()) {
        data_offset = ctx.micro_block_header_->row_count_ * ctx.micro_block_header_->extend_value_bit_;
        data_offset = (data_offset + CHAR_BIT - 1) / CHAR_BIT;
        if (OB_FAIL(set_null_datums_from_fixed_column(
            ctx, row_ids, row_cap, col_data, datums))) {
        }
      } else if (OB_FAIL(set_null_datums_from_var_column(
          ctx, row_index, row_ids, row_cap, datums))) {
      }
    }

    // Fill string data
    const uint16_t string_size = header_->string_size_;
    const static uint16_t min_buf_size = 128;
    const int64_t buf_size = std::max(string_size, min_buf_size);
    char *buf = nullptr;
    bool filled = false;
    if (OB_FAIL(ret)) {
    } else if (OB_ISNULL(buf = static_cast<char *>(ctx.allocator_->alloc(buf_size * row_cap)))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else if (ctx.is_fix_length() && !header_->is_hex_packing()
               && (filled = fill_fixed_diff_rows(*header_, ctx.has_extend_value(),
                   col_data + data_offset, row_ids, row_cap, buf, buf_size, datums))) {
      // rows filled by the fixed length fast path  作者：FDE WorkMate
    } else {
      for (int64_t i = 0; i < row_cap; ++i) {
        header_->copy_string(ObStringDiffHeader::LeftToRight(), std::logical_not<uint8_t>(),
            header_->common_data(), buf + i * buf_size);
      }
    }

    if (OB_FAIL(ret) || filled) {
    } else if (ctx.is_fix_length()) {
      int64_t row_id = 0;
      const unsigned char *cell_data = nullptr;
      char *str_ptr = nullptr;
      for (int64_t i = 0; i < row_cap; ++i) {
        if (ctx.has_extend_value() && datums[i].is_null()) {
          // Skip
        } else {
          row_id = row_ids[i];
          cell_data = col_data + data_offset + row_id * header_->length_;
          str_ptr = buf + i * buf_size;
          if (header_->is_hex_packing()) {
            ObHexStringUnpacker unpacker(header_->hex_char_array(),
                reinterpret_cast<const unsigned char *>(cell_data));
            header_->copy_hex_string(unpacker,
                static_cast<void (ObHexStringUnpacker::*)(unsigned char &)>(&ObHexStringUnpacker::unpack),
                reinterpret_cast<unsigned char *>(str_ptr));
          } else {
            header_->copy_string(
              ObStringDiffHeader::LeftToRight(),
              ObStringDiffHeader::LogicTrue<uint8_t>(),
              cell_data, str_ptr);
          }
          datums[i].pack_ = string_size;
          datums[i].ptr_ = str_ptr;
        }
      }
    } else {
      int64_t row_id = 0;
      const char *row_data = nullptr;
      int64_t row_len = 0;
      const char *cell_data = nullptr;
      int64_t cell_len = 0;
      char *str_ptr = nullptr;
      for (int64_t i = 0; OB_SUCC(ret) && i < row_cap; ++i) {
        row_id = row_ids[i];
        str_ptr = buf + i * buf_size;
        if (ctx.has_extend_value() && datums[i].is_null()) {
          // Do nothing
        } else if (OB_FAIL(locate_row_data(ctx, row_index, row_id, row_data, row_len))) {
        } else if (OB_FAIL(ObRawDecoder::locate_cell_data(cell_data, cell_len, row_data, row_len,
            *ctx.micro_block_header_, *ctx.col_header_, *header_))) {
        } else {
          if (header_->is_hex_packing()) {
            ObHexStringUnpacker unpacker(header_->hex_char_array(),
                reinterpret_cast<const unsigned char *>(cell_data));
            header_->copy_hex_string(unpacker,
                static_cast<void (ObHexStringUnpacker::*)(unsigned char &)>(&ObHexStringUnpacker::unpack),
                reinterpret_cast<unsigned char *>(str_ptr));
          } else {
            header_->copy_string(
              ObStringDiffHeader::LeftToRight(),
              ObStringDiffHeader::LogicTrue<uint8_t>(),
              cell_data, str_ptr);
          }
          datums[i].pack_ = string_size;
          datums[i].ptr_ = str_ptr;
        }
      }
    }
  }
  return ret;
}

} // end namespace blocksstable
} // end namespace oceanbase
