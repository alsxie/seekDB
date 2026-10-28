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

#ifndef _OCEANBASE_STORAGE_FTS_DICT_OB_FT_DAT_DICT_H_
#define _OCEANBASE_STORAGE_FTS_DICT_OB_FT_DAT_DICT_H_

#include "lib/allocator/ob_allocator.h"
#include "lib/ob_errno.h"
#include "lib/utility/ob_macro_utils.h"
#include "ob_ft_dict_def.h"
#include "storage/fts/dict/ob_ft_dict.h"

#include <cstddef>

/* File Description:
 * @brief tools to build a double array trie dictionary.
 */

namespace oceanbase
{
namespace storage
{
/* @class ObFTCharCodeIndex
 * @description: read-only index from a single character to its code, built once after the DAT is
 *   finished.  A key packs the character bytes and the length into one integer, the table has a
 *   power of two capacity with at most 50% load, so a lookup is one multiplication and usually one
 *   probe into a small, dense table.  Characters longer than MAX_KEY_LEN bytes are not indexed and
 *   stay on ObArrayHashMap.  作者：FDE WorkMate
 */
struct ObFTCharCodeIndex final
{
public:
  struct Entry
  {
    uint32_t key_; // 0 means empty
    ObFTWordCode code_;
  } __attribute__((packed));

  static constexpr int64_t MAX_KEY_LEN = 3;
  static constexpr uint32_t MIN_BITS = 4;

  static OB_INLINE bool is_indexable(const int64_t len) { return len > 0 && len <= MAX_KEY_LEN; }

  // Only valid when is_indexable(len). The length byte keeps keys of different lengths apart and
  // makes every key non zero.
  static OB_INLINE uint32_t make_key(const char *ptr, const int64_t len)
  {
    const unsigned char *p = reinterpret_cast<const unsigned char *>(ptr);
    uint32_t key = (static_cast<uint32_t>(len) << 24) | static_cast<uint32_t>(p[0]);
    if (len > 1) {
      key |= static_cast<uint32_t>(p[1]) << 8;
    }
    if (len > 2) {
      key |= static_cast<uint32_t>(p[2]) << 16;
    }
    return key;
  }

  static uint32_t estimate_bits(const uint64_t key_count);
  static size_t estimate_size(const uint32_t bits);

  void init(const uint32_t bits);
  // no duplicate should be inserted
  void insert(const uint32_t key, const ObFTWordCode code);

  OB_INLINE bool find(const uint32_t key, ObFTWordCode &code) const
  {
    bool found = false;
    const uint32_t mask = (1U << bits_) - 1;
    uint32_t pos = slot(key);
    // load factor <= 50%, an empty slot always ends the probe
    while (true) {
      const Entry &entry = entries_[pos];
      if (entry.key_ == key) {
        code = entry.code_;
        found = true;
        break;
      } else if (0 == entry.key_) {
        break;
      } else {
        pos = (pos + 1) & mask;
      }
    }
    return found;
  }

private:
  OB_INLINE uint32_t slot(const uint32_t key) const
  {
    return static_cast<uint32_t>(key * 2654435769U) >> (32 - bits_);
  }

public:
  uint32_t bits_;
  uint32_t count_;
  Entry entries_[1];
} __attribute__((packed));

/* @class ObArrayHashMap
 * @description: a fixed size hash map, used to store words and their code.
 */
class ObArrayHashMap final
{
public:
  struct Entry
  {
    ObFTSingleWord word;
    ObFTWordCode code;
    bool used = false;
  } __attribute__((packed));

  struct Header
  {
    uint32_t buffer_size_;
    uint32_t capacity_;
    uint32_t count_;
    Entry data[1];
  } __attribute__((packed));

  static size_t estimate_capacity(size_t word_num);

  static size_t estimate_size(size_t word_num);

  size_t capacity() const { return header_.capacity_; }

  int init(size_t word_num);

  // no duplicate should be inserted
  int insert(const ObString &key, ObFTWordCode code);

  int find(const ObFTSingleWord &word, ObFTWordCode &code) const;
  int find(const ObString &key, ObFTWordCode &code) const;

  uint32_t count() const { return header_.count_; }
  // number of words that can be put into an ObFTCharCodeIndex
  uint64_t indexable_count() const;
  void fill_char_index(ObFTCharCodeIndex &index) const;

private:
  struct Header header_;
} __attribute__((packed));

/**
 * @class ObFTDAT
 * @brief ObFTDATrie data structure, which is a memory block, which contains
 *          the code map(optional), the base and check array, and the data block(optional).
 */
struct ObFTDAT
{
public:
  static constexpr int32_t FIRST_BASE = 1;
  static constexpr int32_t FIRST_INDEX = 1;
  // all size in bytes
  int64_t mem_block_size_ = 0;
  // node_num_: number of nodes in the trie.
  size_t node_num_ = 0;
  // word_num_: number of words in the trie.
  size_t word_num_ = 0;
  // array_size_ for the base and check array.
  size_t array_size_ = 0;

  // start offset of every data block
  size_t map_offset_ = 0;
  size_t base_offset_ = 0;
  size_t check_offset_ = 0;

  // use if offset is not 0, it means the data is in the data block
  size_t data_index_offset_ = 0;
  // use if offset is not 0
  size_t data_offset_ = 0;
  // offset of the ObFTCharCodeIndex, 0 if there is none (it is always behind the check array)
  size_t char_index_offset_ = 0;
  // a buffer to store dat
  ObFTSingleWord start_word_;
  ObFTSingleWord end_word_;
  char buff[1] = {0};

public:
  ObArrayHashMap *get_map();
  const ObArrayHashMap *get_map() const { return reinterpret_cast<const ObArrayHashMap *>(buff); }
  const ObFTCharCodeIndex *get_char_index() const
  {
    return 0 == char_index_offset_
               ? nullptr
               : reinterpret_cast<const ObFTCharCodeIndex *>(buff + char_index_offset_);
  }
} __attribute__((packed));

template <typename DataType>
class ObFTTrie;

/**
 * @class OBFTDATBuilder
 * @brief oceanbase double array trie builder, build a trie and then a dat.
 */
template <typename DATA_TYPE = void>
class ObFTDATBuilder
{
public:
  ObFTDATBuilder(ObIAllocator &alloc)
      : alloc_(alloc), dat_(nullptr), map_(nullptr), is_inited_(false), next_code_(1)
  {
  }

  ~ObFTDATBuilder() {}

  int init(ObFTTrie<DATA_TYPE> &trie);

  int build_from_trie(ObFTTrie<DATA_TYPE> &trie);

  // get and put to kv_cache
  int get_mem_block(ObFTDAT *&mem, size_t &mem_len)
  {
    mem = dat_;
    if (nullptr == dat_) {
      mem_len = 0;
    } else {
      mem_len = dat_->mem_block_size_;
    }
    return OB_SUCCESS;
  }

private:
  int expand();

  // Append an ObFTCharCodeIndex of all indexable characters behind the check array.
  int build_char_index();

  int encode(const ObString &word, ObFTWordCode &code, bool add);

private:
  common::ObIAllocator &alloc_;
  ObFTDAT *dat_;
  ObArrayHashMap *map_;

  bool is_inited_;
  int32_t next_code_;

private:
  DISALLOW_COPY_AND_ASSIGN(ObFTDATBuilder);
};

/**
 * @class ObFTDATReader
 * @brief Reader to do match on a mem block dat.
 */
template <typename DataType = void>
class ObFTDATReader
{
public:
  ObFTDATReader(ObFTDAT *dat) : dat_(dat) {}

  // Match single word with hit.
  int match_with_hit(const ObString &single_word,
                     const ObDATrieHit &last_hit,
                     ObDATrieHit &hit) const;

private:
  ObFTDAT *dat_;

private:
  DISALLOW_COPY_AND_ASSIGN(ObFTDATReader);
};

} //  namespace storage
} //  namespace oceanbase

#endif // _OCEANBASE_STORAGE_FTS_DICT_OB_FT_DAT_DICT_H_
