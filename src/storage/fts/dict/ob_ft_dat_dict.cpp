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

#define USING_LOG_PREFIX STORAGE_FTS

#include "storage/fts/dict/ob_ft_dat_dict.h"

#include "lib/utility/alloc_assist.h"
#include "lib/alloc/alloc_struct.h"
#include "lib/allocator/ob_allocator.h"
#include "lib/allocator/page_arena.h"
#include "lib/list/ob_list.h"
#include "lib/oblog/ob_log_module.h"
#include "lib/utility/ob_macro_utils.h"
#include "share/rc/ob_server_runtime.h"
#include "storage/fts/dict/ob_ft_trie.h"
namespace oceanbase
{
namespace storage
{
template <typename DATA_TYPE>
int ObFTDATBuilder<DATA_TYPE>::init(ObFTTrie<DATA_TYPE> &trie)
{
  int ret = OB_SUCCESS;
  if (IS_INIT) {
    ret = OB_INIT_TWICE;
  } else {
    size_t map_size = ObArrayHashMap::estimate_size(trie.node_num());
    size_t array_size = trie.node_num() * 6; // by experience.
    size_t base_size = array_size * sizeof(int32_t);
    size_t check_size = base_size;
    size_t total_size = sizeof(ObFTDAT) + map_size + base_size + check_size;

    if (OB_ISNULL(dat_ = static_cast<ObFTDAT *>(alloc_.alloc(total_size)))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      memset(dat_, 0, total_size);
      dat_->mem_block_size_ = total_size;
      dat_->array_size_ = array_size;
      dat_->base_offset_ = map_size;
      dat_->check_offset_ = dat_->base_offset_ + base_size;
      map_ = dat_->get_map();
      if (OB_FAIL(map_->init(trie.node_num()))) {
      }
    }
  }
  return ret;
}

template <typename DATA_TYPE>
int ObFTDATBuilder<DATA_TYPE>::build_from_trie(ObFTTrie<DATA_TYPE> &trie)
{
  typedef typename ObFTTrieNode<DATA_TYPE>::NodeIndex NodeIndex;

  int ret = OB_SUCCESS;
  if (OB_ISNULL(dat_) || OB_ISNULL(dat_->buff)) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    int32_t *base = reinterpret_cast<int32_t *>(dat_->buff + dat_->base_offset_);
    int32_t *check = reinterpret_cast<int32_t *>(dat_->buff + dat_->check_offset_);
    base[ObFTDAT::FIRST_INDEX] = 1;
    ObArenaAllocator alloc(lib::ObMemAttr("Tmp dfs"));
    ObList<ObFTTrieNode<DATA_TYPE> *, ObIAllocator> dfs_queue(alloc);

    // Start to build the DAT.
    trie.root()->dat_build_info_.state_index_ = ObFTDAT::FIRST_BASE;
    dfs_queue.push_back(trie.root());

    int next_base_offset = 0;
    int level = 1;

    while (OB_SUCC(ret) && !dfs_queue.empty()) {
      ObFTTrieNode<DATA_TYPE> *node = dfs_queue.get_first();
      dfs_queue.pop_front();

      int rootIdx = node->dat_build_info_.state_index_;

      // mark as leaf
      if (node->is_leaf_) {
        base[rootIdx] = -1;
      }

      if (level != node->dat_build_info_.level_) {
        level = node->dat_build_info_.level_;
        next_base_offset = 0;
      }

      if (node->is_empty()) {
        // nothing
      } else {
        int try_base = 1;
        try_base += next_base_offset;
        int start = try_base;
        bool need_code = true;
        int try_step = 1;

        for (; try_base < dat_->array_size_; try_base += try_step) {
          bool conflict = false;

          if (node->is_empty()) {
          } else {
            for (typename ObList<NodeIndex, ObIAllocator>::iterator iter = node->children_->begin();
                 iter != node->children_->end();
                 ++iter) {
              ObFTWordCode child_code;
              NodeIndex &child_index = *iter;
              if (need_code) {
                if (OB_FAIL(encode(child_index.word_.get_word(), child_code, true))) {
                  LOG_WARN("Failed to encode");
                  break;
                }
                child_index.child_->dat_build_info_.code_ = child_code;
              }
              child_code = child_index.child_->dat_build_info_.code_;
              if (check[try_base + child_code] != 0) {
                conflict = true;
              }
            }
          }

          if (OB_FAIL(ret)) {
          } else {
            need_code = false;

            if (!conflict) {
              if (node->is_leaf_) {
                base[rootIdx] = -try_base;
              } else {
                base[rootIdx] = try_base;
              }
              break;
            }
            try_step = MAX(1, (try_base - start) / 20);
          }
        }

        if (OB_FAIL(ret) || try_base >= dat_->array_size_) {
          break;
        } else {
          // try_average_.add(try_base + 1 - start);
          if (try_base - start > 100) {
            next_base_offset += try_base - start;
          }

          if (node->is_empty()) {
          } else {
            for (typename ObList<NodeIndex, ObIAllocator>::iterator iter = node->children_->begin();
                 iter != node->children_->end();
                 iter++) {
              typename ObFTTrieNode<DATA_TYPE>::NodeIndex &child_index = *iter;
              int my_index = abs(try_base) + child_index.child_->dat_build_info_.code_;
              // max_used = MAX(max_used, my_index);

              if (my_index >= dat_->array_size_) {
                expand();
                base = reinterpret_cast<int32_t *>(dat_->buff + dat_->base_offset_);
                check = reinterpret_cast<int32_t *>(dat_->buff + dat_->check_offset_);
              }
              child_index.child_->dat_build_info_.state_index_ = my_index;
              check[my_index] = rootIdx;
              dfs_queue.push_back(child_index.child_);
            }
          }
        }
      }

    } // while

    if (OB_FAIL(ret)) {
      // already logged
    } else if (OB_FAIL(trie.get_start_word(dat_->start_word_))) {
    } else if (OB_FAIL(trie.get_end_word(dat_->end_word_))) {
    } else if (OB_FAIL(build_char_index())) {
      LOG_WARN("fail to build char index of dat", K(ret));
    }
    LOG_INFO("build dat finished", K(dat_->start_word_.get_word()), K(dat_->end_word_.get_word()));

    dfs_queue.reset();
    alloc.reset();
  }

  return ret;
}

template <typename DATA_TYPE>
int ObFTDATBuilder<DATA_TYPE>::expand()
{
  int ret = OB_SUCCESS;
  size_t array_size = dat_->array_size_;
  size_t base_inc = array_size * sizeof(int32_t);
  size_t check_inc = array_size * sizeof(int32_t);
  size_t buffer_size = dat_->mem_block_size_ + base_inc + check_inc;

  ObFTDAT *new_dat = nullptr;
  if (OB_ISNULL(new_dat = static_cast<ObFTDAT *>(alloc_.alloc(buffer_size)))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    memset(new_dat, 0, buffer_size);
    new_dat->mem_block_size_ = buffer_size;
    new_dat->array_size_ = array_size * 2;
    new_dat->base_offset_ = dat_->base_offset_;
    new_dat->check_offset_ = dat_->check_offset_ + base_inc;
    // map and base
    MEMCPY(new_dat->buff, dat_->buff, dat_->check_offset_ - 0);
    // check
    size_t check_size = array_size * sizeof(int32_t);
    MEMCPY(new_dat->buff + new_dat->check_offset_, dat_->buff + dat_->check_offset_, check_size);
    alloc_.free(dat_);
    dat_ = new_dat;
    map_ = dat_->get_map();
  }
  return ret;
}

template <typename DATA_TYPE>
int ObFTDATBuilder<DATA_TYPE>::build_char_index()
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(dat_) || OB_ISNULL(map_)) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    const int64_t head_size = dat_->buff - reinterpret_cast<char *>(dat_);
    // everything in use ends with the check array
    const int64_t data_size = dat_->check_offset_ + dat_->array_size_ * sizeof(int32_t);
    // keep the index 8 bytes aligned relative to the block
    const int64_t index_offset = ((head_size + data_size + 7) & ~static_cast<int64_t>(7)) - head_size;
    const uint32_t bits = ObFTCharCodeIndex::estimate_bits(map_->indexable_count());
    const int64_t index_size = ObFTCharCodeIndex::estimate_size(bits);
    const int64_t buffer_size = head_size + index_offset + index_size;
    const int64_t copy_size = MIN(head_size + data_size, dat_->mem_block_size_);
    ObFTDAT *new_dat = nullptr;
    if (OB_UNLIKELY(copy_size > buffer_size)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_ISNULL(new_dat = static_cast<ObFTDAT *>(alloc_.alloc(buffer_size)))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      MEMCPY(new_dat, dat_, copy_size);
      memset(reinterpret_cast<char *>(new_dat) + copy_size, 0, buffer_size - copy_size);
      new_dat->mem_block_size_ = buffer_size;
      new_dat->char_index_offset_ = index_offset;
      ObFTCharCodeIndex *index = reinterpret_cast<ObFTCharCodeIndex *>(new_dat->buff + index_offset);
      index->init(bits);
      new_dat->get_map()->fill_char_index(*index);
      alloc_.free(dat_);
      dat_ = new_dat;
      map_ = dat_->get_map();
    }
  }
  return ret;
}

template <typename DATA_TYPE>
int ObFTDATBuilder<DATA_TYPE>::encode(const ObString &word, ObFTWordCode &code, bool add)
{
  int ret = OB_SUCCESS;
  ObFTWordCode code_value;
  ObFTSingleWord single_word;
  single_word.set_word(word.ptr(), word.length());
  ret = map_->find(single_word, code);
  if (OB_ENTRY_NOT_EXIST == ret) {
    if (add) {
      code = next_code_++;
      ret = map_->insert(word, code);
      if (OB_SUCCESS != ret) {
      }
    } else {
      // do nothing
    }
  } else {
    // nothing
  }
  return ret;
}

template <typename DATA_TYPE>
int ObFTDATReader<DATA_TYPE>::match_with_hit(const ObString &ft_char,
                                             const ObDATrieHit &last_hit,
                                             ObDATrieHit &hit) const
{
  int ret = OB_SUCCESS;
  ObFTWordCode code = 0;
  bool found = false;
  hit.set_unmatch();
  const ObFTCharCodeIndex *index = dat_->get_char_index();
  int32_t *base = reinterpret_cast<int32_t *>(dat_->buff + dat_->base_offset_);
  int32_t *check = reinterpret_cast<int32_t *>(dat_->buff + dat_->check_offset_);

  if (OB_NOT_NULL(index) && ObFTCharCodeIndex::is_indexable(ft_char.length())) {
    // the index holds exactly the indexable words of the map
    found = index->find(ObFTCharCodeIndex::make_key(ft_char.ptr(), ft_char.length()), code);
  } else if (OB_FAIL(dat_->get_map()->find(ft_char, code)) && OB_ENTRY_NOT_EXIST != ret) {
  } else if (OB_ENTRY_NOT_EXIST == ret) {
    ret = OB_SUCCESS;
  } else {
    found = true;
  }

  if (OB_FAIL(ret)) {
  } else if (!found) {
    hit.set_unmatch();
  } else {
    if (&last_hit == &hit) {
      // sameone
    } else {
      hit = last_hit;
    }
    if (0 == hit.base_idx_) {
      hit.base_idx_ = 1; // from current to do check;
    }
    // from current place to do check
    int32_t idx = abs(base[hit.base_idx_]) + code;

    if (OB_UNLIKELY(idx < 0 || idx >= static_cast<int64_t>(dat_->array_size_))) {
      // out of the trie, so no transition
      hit.set_unmatch();
    } else if (check[idx] != hit.base_idx_) {
      hit.set_unmatch();
    } else if (base[idx] < 0) {
      hit.base_idx_ = idx;
      hit.end_pos_ = hit.end_pos_ + ft_char.length();
      hit.char_cnt_++;
      hit.set_match();
      hit.set_prefix(); // to be checked next time
    } else {
      hit.base_idx_ = idx;
      hit.end_pos_ = hit.end_pos_ + ft_char.length();
      hit.char_cnt_++;
      hit.set_prefix();
    }
  }
  return ret;
}

template class ObFTDATBuilder<void>;
template class ObFTDATReader<void>;

ObArrayHashMap *ObFTDAT::get_map() { return reinterpret_cast<ObArrayHashMap *>(buff); }

int ObArrayHashMap::find(const ObFTSingleWord &word, ObFTWordCode &code) const
{
  return find(word.get_word(), code);
}

// Probes with the key bytes directly, so a lookup needs no ObFTSingleWord copy of the key.
// 作者：FDE WorkMate
int ObArrayHashMap::find(const ObString &key, ObFTWordCode &code) const
{
  int ret = OB_ENTRY_NOT_EXIST;
  const uint64_t capacity = header_.capacity_;
  const int64_t key_len = key.length();
  uint64_t index = key.hash() % capacity;
  while (header_.data[index].used) {
    const Entry &entry = header_.data[index];
    if (entry.word.word_len == key_len && 0 == MEMCMP(entry.word.word, key.ptr(), key_len)) {
      code = header_.data[index].code;
      ret = OB_SUCCESS;
      break;
    } else {
      index = (index + 3) % capacity;
    }
  }
  return ret;
}

int ObArrayHashMap::insert(const ObString &key, ObFTWordCode code)
{
  int ret = OB_SUCCESS;
  uint64_t hash = key.hash();
  uint64_t index = hash % header_.capacity_;
  while (header_.data[index].used) {
    index = (index + 3) % header_.capacity_;
  }

  header_.data[index].code = code;
  header_.data[index].word.set_word(key.ptr(), key.length());
  header_.data[index].used = true;
  header_.count_++;
  return ret;
}

int ObArrayHashMap::init(size_t word_num)
{
  int ret = OB_SUCCESS;
  size_t capacity = estimate_capacity(word_num);
  size_t size = estimate_size(word_num);

  memset(this, 0, size);
  header_.buffer_size_ = size;
  header_.capacity_ = capacity;
  header_.count_ = 0;
  return OB_SUCCESS;
}

size_t ObArrayHashMap::estimate_size(size_t word_num)
{
  size_t capacity = estimate_capacity(word_num);
  size_t size = sizeof(Header) + capacity * sizeof(Entry);
  return size;
}

size_t ObArrayHashMap::estimate_capacity(size_t word_num)
{
  constexpr size_t MIN_CAPACITY = 101;                   // prime
  size_t capacity = MAX(word_num * 4 / 3, MIN_CAPACITY); // 75%
  return capacity;
}

uint64_t ObArrayHashMap::indexable_count() const
{
  uint64_t cnt = 0;
  for (uint64_t i = 0; i < header_.capacity_; ++i) {
    const Entry &entry = header_.data[i];
    if (entry.used && ObFTCharCodeIndex::is_indexable(entry.word.word_len)) {
      ++cnt;
    }
  }
  return cnt;
}

void ObArrayHashMap::fill_char_index(ObFTCharCodeIndex &index) const
{
  for (uint64_t i = 0; i < header_.capacity_; ++i) {
    const Entry &entry = header_.data[i];
    if (entry.used && ObFTCharCodeIndex::is_indexable(entry.word.word_len)) {
      index.insert(ObFTCharCodeIndex::make_key(entry.word.word, entry.word.word_len), entry.code);
    }
  }
}

uint32_t ObFTCharCodeIndex::estimate_bits(const uint64_t key_count)
{
  uint32_t bits = MIN_BITS;
  // keep at most half of the slots used
  while (bits < 31 && (1ULL << bits) < 2 * key_count) {
    ++bits;
  }
  return bits;
}

size_t ObFTCharCodeIndex::estimate_size(const uint32_t bits)
{
  return sizeof(ObFTCharCodeIndex) + ((1ULL << bits) - 1) * sizeof(Entry);
}

void ObFTCharCodeIndex::init(const uint32_t bits)
{
  bits_ = bits;
  count_ = 0;
  memset(entries_, 0, (1ULL << bits) * sizeof(Entry));
}

void ObFTCharCodeIndex::insert(const uint32_t key, const ObFTWordCode code)
{
  const uint32_t mask = (1U << bits_) - 1;
  uint32_t pos = slot(key);
  while (0 != entries_[pos].key_) {
    pos = (pos + 1) & mask;
  }
  entries_[pos].key_ = key;
  entries_[pos].code_ = code;
  ++count_;
}
} //  namespace storage
} //  namespace oceanbase
