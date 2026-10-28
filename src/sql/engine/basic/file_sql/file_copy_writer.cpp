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

#include "sql/engine/basic/file_sql/file_copy_writer.h"
#include "lib/time/ob_time_utility.h"

#include <cerrno>
#include <climits>
#include <cstdio>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace oceanbase
{
using namespace common;
namespace sql
{
namespace file_sql
{

static const int64_t COPY_DESC_VERSION = 1;
static const int64_t COPY_WRITE_BUFFER_SIZE = 1LL << 20;

int CopyDesc::serialize(ObIAllocator &allocator, ObString &out) const
{
  int ret = OB_SUCCESS;
  int64_t buf_len = 128 + path_.length() + secure_file_priv_.length() + null_str_.length();
  for (int64_t i = 0; i < columns_.count(); ++i) {
    buf_len += 16 + columns_.at(i).length();
  }
  char *buf = static_cast<char *>(allocator.alloc(buf_len));
  int64_t pos = 0;
  if (OB_ISNULL(buf)) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  }
  FS_ENCODE_INT(COPY_DESC_VERSION);
  FS_ENCODE_STR(path_);
  FS_ENCODE_STR(secure_file_priv_);
  FS_ENCODE_INT(header_);
  FS_ENCODE_INT(delim_);
  FS_ENCODE_INT(quote_);
  FS_ENCODE_INT(escape_);
  FS_ENCODE_STR(null_str_);
  FS_ENCODE_INT(columns_.count());
  for (int64_t i = 0; OB_SUCC(ret) && i < columns_.count(); ++i) {
    FS_ENCODE_STR(columns_.at(i));
  }
  if (OB_SUCC(ret)) {
    out.assign_ptr(buf, static_cast<int32_t>(pos));
  }
  return ret;
}

int CopyDesc::deserialize(const ObString &in)
{
  int ret = OB_SUCCESS;
  const char *buf = in.ptr();
  const int64_t data_len = in.length();
  int64_t pos = 0;
  int64_t version = 0;
  int64_t count = 0;
  columns_.reset();
  FS_DECODE_INT(version, int64_t);
  if (OB_SUCC(ret) && COPY_DESC_VERSION != version) {
    ret = OB_DESERIALIZE_ERROR;
    LOG_WARN("unexpected copy desc version", K(ret), K(version));
  }
  FS_DECODE_STR(path_);
  FS_DECODE_STR(secure_file_priv_);
  FS_DECODE_INT(header_, bool);
  FS_DECODE_INT(delim_, char);
  FS_DECODE_INT(quote_, char);
  FS_DECODE_INT(escape_, char);
  FS_DECODE_STR(null_str_);
  FS_DECODE_INT(count, int64_t);
  if (OB_SUCC(ret) && (count < 0 || count > FILE_SQL_MAX_COLUMNS * 4)) {
    ret = OB_DESERIALIZE_ERROR;
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < count; ++i) {
    ObString name;
    FS_DECODE_STR(name);
    if (OB_SUCC(ret) && OB_FAIL(columns_.push_back(name))) {
    }
  }
  return ret;
}

static int copy_option_error(const FileOption &opt, const char *reason)
{
  int ret = OB_NOT_SUPPORTED;
  FORWARD_USER_ERROR_MSG(ret, "COPY option '%.*s' %s", opt.name_.length(), opt.name_.ptr(), reason);
  return ret;
}

static int get_copy_char(const FileOption &opt, const bool allow_empty, char &c)
{
  int ret = OB_SUCCESS;
  if (FileOptionValueType::STRING != opt.type_) {
    ret = copy_option_error(opt, "expects a string value");
  } else if (0 == opt.str_.length() && allow_empty) {
    c = '\0';
  } else if (1 != opt.str_.length()) {
    ret = copy_option_error(opt, "only supports a single byte character");
  } else if ('\n' == opt.str_.ptr()[0] || '\r' == opt.str_.ptr()[0]) {
    ret = copy_option_error(opt, "can not be a line break");
  } else {
    c = opt.str_.ptr()[0];
  }
  return ret;
}

int resolve_copy_target(ObIAllocator &allocator, const ObString &target,
                        const ObString &secure_file_priv, ObString &real_path)
{
  int ret = OB_SUCCESS;
  char path_buf[PATH_MAX + 1];
  char dir_buf[PATH_MAX + 1];
  char real_dir[PATH_MAX + 1];
  char full[PATH_MAX + 1];
  const char *name = NULL;
  bool allowed = false;
  struct stat st;
  if (target.empty() || target.length() > PATH_MAX) {
    ret = OB_INVALID_ARGUMENT;
    FORWARD_USER_ERROR_MSG(ret, "invalid COPY target '%.*s'", target.length(), target.ptr());
  } else {
    MEMCPY(path_buf, target.ptr(), target.length());
    path_buf[target.length()] = '\0';
    char *slash = ::strrchr(path_buf, '/');
    if (NULL == slash) {
      MEMCPY(dir_buf, ".", 2);
      name = path_buf;
    } else {
      const int64_t dir_len = slash - path_buf;
      if (0 == dir_len) {
        MEMCPY(dir_buf, "/", 2);
      } else {
        MEMCPY(dir_buf, path_buf, dir_len);
        dir_buf[dir_len] = '\0';
      }
      name = slash + 1;
    }
    if ('\0' == name[0] || 0 == ::strcmp(name, ".") || 0 == ::strcmp(name, "..")) {
      ret = OB_INVALID_ARGUMENT;
      FORWARD_USER_ERROR_MSG(ret, "invalid COPY target '%.*s'", target.length(), target.ptr());
    } else if (NULL == ::realpath(dir_buf, real_dir)) {
      ret = OB_FILE_NOT_EXIST;
      FORWARD_USER_ERROR_MSG(ret, "directory of COPY target '%.*s' does not exist",
                             target.length(), target.ptr());
    } else if (::snprintf(full, sizeof(full), "%s%s%s", real_dir,
                          ('/' == real_dir[STRLEN(real_dir) - 1]) ? "" : "/", name) >= static_cast<int>(sizeof(full))) {
      ret = OB_SIZE_OVERFLOW;
      FORWARD_USER_ERROR_MSG(ret, "COPY target path is too long");
    } else if (OB_FAIL(check_path_in_secure_dir(ObString(full), secure_file_priv, allowed))) {
    } else if (!allowed) {
      ret = OB_ERR_NO_PRIVILEGE;
      FORWARD_USER_ERROR_MSG(ret, "access to '%.*s' is denied, the file must be inside secure_file_priv",
                             target.length(), target.ptr());
    } else if (0 == ::lstat(full, &st)) {
      ret = OB_FILE_ALREADY_EXIST;
      FORWARD_USER_ERROR_MSG(ret, "COPY target '%.*s' already exists", target.length(), target.ptr());
    } else if (OB_FAIL(ob_write_string(allocator, ObString(full), real_path))) {
    }
  }
  return ret;
}

int build_copy_desc(ObIAllocator &allocator, const ObString &target,
                    const ObIArray<FileOption> &options, const ObString &secure_file_priv,
                    CopyDesc &desc)
{
  int ret = OB_SUCCESS;
  bool escape_set = false;
  for (int64_t i = 0; OB_SUCC(ret) && i < options.count(); ++i) {
    const FileOption &opt = options.at(i);
    const ObString &n = opt.name_;
    for (int64_t j = 0; OB_SUCC(ret) && j < i; ++j) {
      if (0 == options.at(j).name_.case_compare(n)) {
        ret = copy_option_error(opt, "is specified more than once");
      }
    }
    if (OB_FAIL(ret)) {
    } else if (0 == n.case_compare("format")) {
      if ((FileOptionValueType::STRING != opt.type_ && FileOptionValueType::IDENT != opt.type_)
          || 0 != opt.str_.case_compare("csv")) {
        ret = copy_option_error(opt, "only supports csv");
      }
    } else if (0 == n.case_compare("header")) {
      if (FileOptionValueType::NONE == opt.type_) {
        desc.header_ = true;
      } else if (FileOptionValueType::BOOL == opt.type_) {
        desc.header_ = opt.bool_;
      } else if (FileOptionValueType::INT == opt.type_ && (0 == opt.int_ || 1 == opt.int_)) {
        desc.header_ = (1 == opt.int_);
      } else {
        ret = copy_option_error(opt, "expects a boolean value");
      }
    } else if (0 == n.case_compare("delimiter") || 0 == n.case_compare("delim")
               || 0 == n.case_compare("sep")) {
      ret = get_copy_char(opt, false, desc.delim_);
    } else if (0 == n.case_compare("quote")) {
      ret = get_copy_char(opt, false, desc.quote_);
    } else if (0 == n.case_compare("escape")) {
      escape_set = true;
      ret = get_copy_char(opt, false, desc.escape_);
    } else if (0 == n.case_compare("nullstr") || 0 == n.case_compare("null")) {
      if (FileOptionValueType::STRING != opt.type_) {
        ret = copy_option_error(opt, "expects a string value");
      } else if (OB_FAIL(ob_write_string(allocator, opt.str_, desc.null_str_))) {
      }
    } else {
      ret = copy_option_error(opt, "is not supported");
    }
  }
  if (OB_FAIL(ret)) {
  } else if (!escape_set) {
    desc.escape_ = desc.quote_;
  }
  if (OB_FAIL(ret)) {
  } else if (desc.delim_ == desc.quote_ || desc.delim_ == desc.escape_) {
    ret = OB_NOT_SUPPORTED;
    FORWARD_USER_ERROR_MSG(ret, "COPY delimiter must differ from quote and escape");
  } else if (OB_FAIL(ob_write_string(allocator, secure_file_priv, desc.secure_file_priv_))) {
  } else if (OB_FAIL(resolve_copy_target(allocator, target, secure_file_priv, desc.path_))) {
  }
  return ret;
}

CopyCsvWriter::CopyCsvWriter()
  : desc_(nullptr), fd_(-1), buf_(nullptr), buf_len_(0), pos_(0), field_idx_(0), committed_(false),
    allocator_("FileSqlCopy")
{
  tmp_path_[0] = '\0';
}

int CopyCsvWriter::open(const CopyDesc &desc)
{
  int ret = OB_SUCCESS;
  ObString real_path;
  ObArenaAllocator tmp_alloc("FileSqlCopy");
  if (OB_UNLIKELY(fd_ >= 0)) {
    ret = OB_INIT_TWICE;
  } else if (OB_FAIL(resolve_copy_target(tmp_alloc, desc.path_, desc.secure_file_priv_, real_path))) {
    // the target may have been created after the plan was generated
    LOG_WARN("COPY target is not writable", K(ret), K(desc.path_));
  } else if (OB_ISNULL(buf_ = static_cast<char *>(allocator_.alloc(COPY_WRITE_BUFFER_SIZE)))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    buf_len_ = COPY_WRITE_BUFFER_SIZE;
    desc_ = &desc;
    for (int64_t attempt = 0; OB_SUCC(ret) && fd_ < 0 && attempt < 16; ++attempt) {
      const char *last = static_cast<const char *>(::memrchr(real_path.ptr(), '/', real_path.length()));
      const int64_t dir_len = OB_ISNULL(last) ? 0 : last - real_path.ptr();
      const ObString name(real_path.length() - dir_len - 1, last + 1);
      if (::snprintf(tmp_path_, sizeof(tmp_path_), "%.*s/.%.*s.%d.%ld.%ld.tmp",
                     static_cast<int>(dir_len), real_path.ptr(), name.length(), name.ptr(),
                     static_cast<int>(::getpid()), ObTimeUtility::current_time(), attempt)
          >= static_cast<int>(sizeof(tmp_path_))) {
        ret = OB_SIZE_OVERFLOW;
        FORWARD_USER_ERROR_MSG(ret, "COPY target path is too long");
      } else if ((fd_ = ::open(tmp_path_, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644)) < 0) {
        if (EEXIST != errno) {
          ret = OB_IO_ERROR;
          FORWARD_USER_ERROR_MSG(ret, "failed to create COPY file in '%.*s', errno %d",
                                 static_cast<int>(dir_len), real_path.ptr(), errno);
        }
      }
    }
    if (OB_SUCC(ret) && fd_ < 0) {
      ret = OB_IO_ERROR;
      tmp_path_[0] = '\0';
    } else if (OB_FAIL(ret)) {
      tmp_path_[0] = '\0';
    }
  }
  if (OB_SUCC(ret) && desc.header_) {
    if (OB_FAIL(begin_row())) {
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < desc.columns_.count(); ++i) {
      ret = append_field(desc.columns_.at(i).ptr(), desc.columns_.at(i).length(), false);
    }
    if (OB_SUCC(ret)) {
      ret = end_row();
    }
  }
  if (OB_FAIL(ret)) {
    abort();
  }
  return ret;
}

int CopyCsvWriter::append_raw(const char *str, const int64_t len)
{
  int ret = OB_SUCCESS;
  int64_t done = 0;
  while (OB_SUCC(ret) && done < len) {
    if (pos_ == buf_len_ && OB_FAIL(flush())) {
    } else {
      const int64_t n = MIN(len - done, buf_len_ - pos_);
      MEMCPY(buf_ + pos_, str + done, n);
      pos_ += n;
      done += n;
    }
  }
  return ret;
}

int CopyCsvWriter::flush()
{
  int ret = OB_SUCCESS;
  int64_t written = 0;
  while (OB_SUCC(ret) && written < pos_) {
    const ssize_t n = ::write(fd_, buf_ + written, pos_ - written);
    if (n < 0) {
      if (EINTR != errno) {
        ret = OB_IO_ERROR;
        FORWARD_USER_ERROR_MSG(ret, "failed to write COPY file, errno %d", errno);
      }
    } else {
      written += n;
    }
  }
  if (OB_SUCC(ret)) {
    pos_ = 0;
  }
  return ret;
}

bool CopyCsvWriter::need_quote(const char *str, const int64_t len) const
{
  // An empty unquoted field reads back as NULL, so empty strings are quoted.
  bool need = (0 == len);
  if (!need && len == desc_->null_str_.length() && 0 == MEMCMP(str, desc_->null_str_.ptr(), len)) {
    need = true;
  }
  for (int64_t i = 0; !need && i < len; ++i) {
    const char c = str[i];
    need = (c == desc_->delim_ || c == desc_->quote_ || c == desc_->escape_ || '\n' == c || '\r' == c);
  }
  if (!need && len > 0) {
    need = (' ' == str[0] || '\t' == str[0] || ' ' == str[len - 1] || '\t' == str[len - 1]);
  }
  return need;
}

int CopyCsvWriter::begin_row()
{
  field_idx_ = 0;
  return fd_ < 0 ? OB_NOT_INIT : OB_SUCCESS;
}

int CopyCsvWriter::append_field(const char *str, const int64_t len, const bool is_null)
{
  int ret = OB_SUCCESS;
  if (field_idx_ > 0 && OB_FAIL(append_raw(&desc_->delim_, 1))) {
  } else if (is_null) {
    ret = append_raw(desc_->null_str_.ptr(), desc_->null_str_.length());
  } else if (!need_quote(str, len)) {
    ret = append_raw(str, len);
  } else if (OB_FAIL(append_raw(&desc_->quote_, 1))) {
  } else {
    int64_t start = 0;
    for (int64_t i = 0; OB_SUCC(ret) && i < len; ++i) {
      const char c = str[i];
      if (c == desc_->quote_ || (c == desc_->escape_ && desc_->escape_ != desc_->quote_)) {
        if (OB_FAIL(append_raw(str + start, i - start))) {
        } else if (OB_FAIL(append_raw(&desc_->escape_, 1))) {
        } else {
          start = i;
        }
      }
    }
    if (OB_SUCC(ret) && OB_FAIL(append_raw(str + start, len - start))) {
    } else if (OB_SUCC(ret)) {
      ret = append_raw(&desc_->quote_, 1);
    }
  }
  if (OB_SUCC(ret)) {
    ++field_idx_;
  }
  return ret;
}

int CopyCsvWriter::end_row()
{
  return append_raw("\n", 1);
}

int CopyCsvWriter::commit()
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(fd_ < 0) || OB_ISNULL(desc_)) {
    ret = OB_NOT_INIT;
  } else if (OB_FAIL(flush())) {
  } else if (0 != ::fsync(fd_)) {
    ret = OB_IO_ERROR;
    FORWARD_USER_ERROR_MSG(ret, "failed to sync COPY file, errno %d", errno);
  } else if (0 != ::close(fd_)) {
    fd_ = -1;
    ret = OB_IO_ERROR;
    FORWARD_USER_ERROR_MSG(ret, "failed to close COPY file, errno %d", errno);
  } else {
    fd_ = -1;
    char target[PATH_MAX + 1];
    const int64_t len = MIN(desc_->path_.length(), PATH_MAX);
    MEMCPY(target, desc_->path_.ptr(), len);
    target[len] = '\0';
    // link() never replaces an existing file, unlike rename().
    if (0 != ::link(tmp_path_, target)) {
      if (EEXIST == errno) {
        ret = OB_FILE_ALREADY_EXIST;
        FORWARD_USER_ERROR_MSG(ret, "COPY target '%s' already exists", target);
      } else {
        ret = OB_IO_ERROR;
        FORWARD_USER_ERROR_MSG(ret, "failed to publish COPY file '%s', errno %d", target, errno);
      }
    } else {
      committed_ = true;
    }
  }
  if (OB_FAIL(ret)) {
    abort();
  } else {
    ::unlink(tmp_path_);
    tmp_path_[0] = '\0';
  }
  return ret;
}

void CopyCsvWriter::abort()
{
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
  if ('\0' != tmp_path_[0]) {
    ::unlink(tmp_path_);
    tmp_path_[0] = '\0';
  }
  pos_ = 0;
}

}  // namespace file_sql
}  // namespace sql
}  // namespace oceanbase
