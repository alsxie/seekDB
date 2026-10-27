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
// COPY (query) TO 'file' (format csv, header, delimiter ',', quote '"',
// escape '"', nullstr '') support:
//   * CopyDesc: validated options and the canonical target path, serialized
//     into the plan.
//   * CopyCsvWriter: RFC 4180 writer whose output is readable by read_csv.
//     Rows go to an O_EXCL temp file in the target directory which is linked
//     to the target name only on success (no replace), so a failed query or
//     an existing target never leaves a partial file behind.

#pragma once

#include "lib/allocator/page_arena.h"
#include "sql/engine/basic/file_sql/file_scanner.h"

namespace oceanbase
{
namespace sql
{
namespace file_sql
{

struct CopyDesc
{
  CopyDesc() : path_(), secure_file_priv_(), header_(false), delim_(','), quote_('"'),
               escape_('"'), null_str_(), columns_() {}
  int serialize(common::ObIAllocator &allocator, common::ObString &out) const;
  int deserialize(const common::ObString &in);
  TO_STRING_KV(K_(path), K_(header), K_(delim), K_(quote), K_(escape), K_(null_str), K_(columns));
  common::ObString path_;               // canonical target path
  common::ObString secure_file_priv_;
  bool header_;
  char delim_;
  char quote_;
  char escape_;
  common::ObString null_str_;
  common::ObSEArray<common::ObString, 16> columns_;  // header names
};

// Validates COPY options and the target path (its directory must exist inside
// secure_file_priv and the target itself must not exist).
int build_copy_desc(common::ObIAllocator &allocator, const common::ObString &target,
                    const common::ObIArray<FileOption> &options,
                    const common::ObString &secure_file_priv, CopyDesc &desc);

// Resolves the target (which does not exist yet) to <realpath(dir)>/<name>.
int resolve_copy_target(common::ObIAllocator &allocator, const common::ObString &target,
                        const common::ObString &secure_file_priv, common::ObString &real_path);

class CopyCsvWriter
{
public:
  CopyCsvWriter();
  ~CopyCsvWriter() { abort(); }
  int open(const CopyDesc &desc);
  int begin_row();
  int append_field(const char *str, const int64_t len, const bool is_null);
  int end_row();
  // Flushes, syncs and publishes the file under its final name.
  int commit();
  // Drops the temp file. Safe to call at any time.
  void abort();
  bool is_committed() const { return committed_; }
private:
  bool need_quote(const char *str, const int64_t len) const;
  int append_raw(const char *str, const int64_t len);
  int flush();
  const CopyDesc *desc_;
  int fd_;
  char tmp_path_[PATH_MAX + 1];
  char *buf_;
  int64_t buf_len_;
  int64_t pos_;
  int64_t field_idx_;
  bool committed_;
  common::ObArenaAllocator allocator_;
};

}  // namespace file_sql
}  // namespace sql
}  // namespace oceanbase
