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

#include "object/ob_object.h"
#define USING_LOG_PREFIX STORAGE_FTS

#include "share/rc/ob_server_runtime.h"
#include "storage/fts/ob_fts_stop_word.h"
#include "storage/fts/ob_fts_parser_helper.h"
#include "storage/fts/ob_fts_parser_property.h"

namespace oceanbase
{
namespace storage
{

namespace
{
// The stop word set never changes after init, but ObHashSet's default defend mode takes a bucket
// read lock on every lookup.  A second set with the same bucket number, the same words in the same
// order and NoPthreadDefendMode gives exactly the same answers without the lock.  It is built
// once per process and never freed (it may be used until exit).  作者：FDE WorkMate
typedef common::hash::ObHashSet<ObFTWord, common::hash::NoPthreadDefendMode> ObNoLockStopWordSet;

ObNoLockStopWordSet *build_no_lock_stopword_set(const int64_t bucket_num, const ObObjMeta &meta)
{
  int ret = OB_SUCCESS;
  ObNoLockStopWordSet *set = nullptr;
  void *buf = nullptr;
  const lib::ObMemAttr attr("StopWordSet");
  if (OB_ISNULL(buf = ob_malloc(sizeof(ObNoLockStopWordSet), attr))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
    LOG_WARN("fail to alloc no lock stop word set", K(ret));
  } else if (FALSE_IT(set = new (buf) ObNoLockStopWordSet())) {
  } else if (OB_FAIL(set->create(bucket_num, "StopWordSet", "StopWordSet"))) {
    LOG_WARN("fail to create no lock stop word set", K(ret), K(bucket_num));
  } else {
    const int64_t stopword_count = sizeof(ob_stop_word_list) / sizeof(ob_stop_word_list[0]);
    for (int64_t i = 0; OB_SUCC(ret) && i < stopword_count; ++i) {
      ObFTWord stopword(STRLEN(ob_stop_word_list[i]), ob_stop_word_list[i], meta);
      if (OB_FAIL(set->set_refactored(stopword))) {
        LOG_WARN("fail to add stop word", K(ret), K(i));
      }
    }
  }
  if (OB_FAIL(ret) && OB_NOT_NULL(set)) {
    set->~ObNoLockStopWordSet();
    ob_free(buf);
    set = nullptr;
  }
  return set;
}

// nullptr if it could not be built, the locked set is used then
const ObNoLockStopWordSet *get_no_lock_stopword_set(const int64_t bucket_num, const ObObjMeta &meta)
{
  static const ObNoLockStopWordSet *set = build_no_lock_stopword_set(bucket_num, meta);
  return set;
}
} // namespace

////////////////////////////////////////////////////////////////////////////////
// class ObStopWordChecker
ObStopWordChecker::~ObStopWordChecker()
{
  destroy();
}

int ObStopWordChecker::init()
{
  int ret = OB_SUCCESS;
  
  

  if (inited_) {
    ret = OB_INIT_TWICE;
  } else if (OB_FAIL(stopword_set_.create(DEFAULT_STOPWORD_BUCKET_NUM, "StopWordSet", "StopWordSet"))) {
  } else {
    ObObjMeta stop_meta;
    stop_meta.set_varchar();
    stop_meta.set_collation_type(ObCollationType::CS_TYPE_UTF8MB4_GENERAL_CI);

    stopword_type_.set_meta(stop_meta);
    const int64_t stopword_count = sizeof(ob_stop_word_list) / sizeof(ob_stop_word_list[0]);
    for (int64_t i = 0; OB_SUCC(ret) && i < stopword_count; ++i) {
      ObFTWord stopword(STRLEN(ob_stop_word_list[i]), ob_stop_word_list[i], stopword_type_);
      if (OB_FAIL(stopword_set_.set_refactored(stopword))) {
      }
    }

    if (OB_SUCC(ret)) {
      inited_ = true;
    }
  }
  return ret;
}

void ObStopWordChecker::destroy()
{
  if (inited_) {
    stopword_set_.destroy();
    inited_ = false;
  }
}

int ObStopWordChecker::check_stopword(const ObFTWord &word, bool &is_stopword)
{
  int ret = OB_SUCCESS;
  
  
  common::ObArenaAllocator allocator(lib::ObMemAttr("ChkStopWord"));
  if (OB_UNLIKELY(!inited_)) {
    ret = OB_NOT_INIT;
  } else if (OB_UNLIKELY(word.empty())) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    common::ObString cmp_str;
    // do nothing set out with in if type is the same.
    if (OB_FAIL(common::ObCharset::charset_convert(
                                       allocator,
                                       word.get_word().get_string(),
                                       word.get_collation_type(),
                                       stopword_type_.get_collation_type(),
                                       cmp_str))) {
    } else {
      ObFTWord converted(cmp_str.length(), cmp_str.ptr(), stopword_type_);
      const ObNoLockStopWordSet *no_lock_set =
          get_no_lock_stopword_set(DEFAULT_STOPWORD_BUCKET_NUM, stopword_type_);
      if (OB_NOT_NULL(no_lock_set)) {
        ret = no_lock_set->exist_refactored(converted);
      } else {
        ret = stopword_set_.exist_refactored(converted);
      }
      if (OB_HASH_NOT_EXIST == ret) {
        is_stopword = false;
        ret = OB_SUCCESS;
      } else if (OB_HASH_EXIST == ret) {
        is_stopword = true;
        ret = OB_SUCCESS;
      } else if (OB_SUCC(ret)) {
        ret = OB_ERR_UNEXPECTED;
      } else {
      }
    }
  }
  return ret;
}

////////////////////////////////////////////////////////////////////////////////
// class ObAddWord
ObAddWord::ObAddWord(
    const ObFTParserProperty &property,
    const ObObjMeta &meta,
    const ObAddWordFlag &flag,
    common::ObIAllocator &allocator,
    ObFTWordMap &word_map)
  : word_meta_(meta),
    allocator_(allocator),
    word_map_(word_map),
    min_max_word_cnt_(0),
    non_stopword_cnt_(0),
    stopword_cnt_(0),
    min_token_size_(property.min_token_size_),
    max_token_size_(property.max_token_size_),
    flag_(flag)
{
}

int ObAddWord::process_word(
    const char *word,
    const int64_t word_len,
    const int64_t char_cnt,
    const int64_t word_freq)
{
  int ret = OB_SUCCESS;
  bool is_stopword = false;
  ObFTWord src_word(word_len, word, word_meta_);
  ObFTWord dst_word;
  if (OB_ISNULL(word) || OB_UNLIKELY(0 >= word_len || 0 >= char_cnt || 0 >= word_freq)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (is_min_max_word(char_cnt)) {
    ++min_max_word_cnt_;
  } else if (OB_FAIL(casedown_word(src_word, dst_word))) {
  } else if (OB_FAIL(check_stopword(dst_word, is_stopword))) {
  } else if (OB_UNLIKELY(is_stopword)) {
    ++stopword_cnt_;
  } else if (OB_FAIL(groupby_word(dst_word, word_freq))) {
  } else {
    non_stopword_cnt_ += word_freq;
  }
  return ret;
}

bool ObAddWord::is_min_max_word(const int64_t c_len) const
{
  return flag_.min_max_word() && (c_len < min_token_size_ || c_len > max_token_size_);
}

int ObAddWord::casedown_word(const ObFTWord &src, ObFTWord &dst)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(src.empty())) {
    ret = OB_INVALID_ARGUMENT;
  } else if (flag_.casedown()) {
    ObString dst_str;
    if (OB_FAIL(ObCharset::tolower(
                    word_meta_.get_collation_type(),
                    src.get_word().get_string(),
                    dst_str,
                    allocator_))) {
    } else {
      ObFTWord tmp(dst_str.length(), dst_str.ptr(), word_meta_);
      dst = tmp;
    }
  } else {
    dst = src;
  }
  return ret;
}

int ObAddWord::check_stopword(const ObFTWord &ft_word, bool &is_stopword)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(ft_word.empty())) {
    ret = OB_INVALID_ARGUMENT;
  } else if (flag_.stopword()) {
    ObStopWordChecker *stop_word_checker = ObFTParseData::instance().stop_word_checker();
    if (OB_ISNULL(stop_word_checker)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(stop_word_checker->check_stopword(ft_word, is_stopword))) {
    }
  }
  return ret;
}

namespace
{
// Applied to an existing entry: same result as get + set with overwrite, i.e. the entry keeps its
// place in the bucket, the newest word becomes the key and the count grows by word_freq.
// 作者：FDE WorkMate
struct ObFTWordGroupbyOp final
{
  ObFTWordGroupbyOp(const ObFTWord &word, const int64_t word_freq) : word_(word), word_freq_(word_freq) {}
  void operator()(common::hash::HashMapPair<ObFTWord, int64_t> &entry)
  {
    entry.first = word_;
    entry.second += word_freq_;
  }
  const ObFTWord &word_;
  const int64_t word_freq_;
};
} // namespace

int ObAddWord::groupby_word(const ObFTWord &word, const int64_t word_freq)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(word.empty() || word_freq <= 0)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (!flag_.groupby_word()) {
    if (OB_FAIL(word_map_.set_refactored(word, 1/*word count*/))) {
    }
  } else {
    // one hash and one bucket walk instead of get + set; a new word starts with count 1
    ObFTWordGroupbyOp groupby_op(word, word_freq);
    if (OB_FAIL(word_map_.set_or_update(word, 1/*word count*/, groupby_op))) {
    }
  }
  return ret;
}

} // end namespace storage
} // end namespace oceanbase
