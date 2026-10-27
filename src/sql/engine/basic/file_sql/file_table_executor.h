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
// Runtime side of file tables: owns the scanner of one file table operator
// and writes scanned values into the projected column expressions.

#pragma once

#include "lib/allocator/page_arena.h"
#include "sql/engine/basic/file_sql/file_scanner.h"

namespace oceanbase
{
namespace sql
{
class ObExpr;
class ObEvalCtx;
class ObJsonTableSpec;
namespace file_sql
{

class FileTableExecutor
{
public:
  FileTableExecutor();
  ~FileTableExecutor() { destroy(); }
  // `spec.namespace_def_[0]` carries the serialized FileScanDesc, column i
  // of the file maps to `spec.cols_def_[i + 1]`.
  int init(const ObJsonTableSpec &spec);
  int open();
  int rescan();
  int get_next_row(ObEvalCtx &eval_ctx);
  void close();
  void destroy();
  const FileScanDesc &get_desc() const { return desc_; }
private:
  int fill_datum(const int64_t col_idx, ObEvalCtx &eval_ctx);
  common::ObArenaAllocator allocator_;
  FileScanDesc desc_;
  FileScanner *scanner_;
  common::ObSEArray<FileValue, 16> row_;
  common::ObSEArray<ObExpr *, 16> out_exprs_;
  common::ObSEArray<bool, 16> projected_;
  bool inited_;
  bool opened_;
};

}  // namespace file_sql
}  // namespace sql
}  // namespace oceanbase
