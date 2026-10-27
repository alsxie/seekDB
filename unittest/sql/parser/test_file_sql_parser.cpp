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
// Parser tests for file table functions (read_csv / read_json / read_parquet /
// glob), DESCRIBE over a query and COPY ... TO.

#include <gtest/gtest.h>
#include "sql/parser/ob_parser.h"
#include "sql/parser/parse_node.h"

using namespace oceanbase::common;
using namespace oceanbase::sql;

namespace
{

class FileSqlParserTest : public ::testing::Test
{
protected:
  int parse(const char *sql, ParseResult &result)
  {
    ObParser parser(allocator_, SMO_DEFAULT);
    memset(&result, 0, sizeof(result));
    return parser.parse(ObString(sql), result);
  }
  // Depth first search for the first node of `type`.
  static const ParseNode *find(const ParseNode *node, const ObItemType type)
  {
    const ParseNode *found = nullptr;
    if (nullptr != node) {
      if (type == node->type_) {
        found = node;
      }
      for (int64_t i = 0; nullptr == found && i < node->num_child_; ++i) {
        found = find(node->children_[i], type);
      }
    }
    return found;
  }
  static ObString node_str(const ParseNode *node)
  {
    return nullptr == node ? ObString() : ObString(static_cast<int32_t>(node->str_len_), node->str_value_);
  }
  const ParseNode *parse_file_table(const char *sql)
  {
    ParseResult result;
    EXPECT_EQ(OB_SUCCESS, parse(sql, result)) << sql;
    const ParseNode *node = find(result.result_tree_, T_FILE_TABLE_EXPRESSION);
    EXPECT_NE(nullptr, node) << sql;
    return node;
  }
  ObArenaAllocator allocator_;
};

TEST_F(FileSqlParserTest, table_function_basic)
{
  const ParseNode *node = parse_file_table("select * from read_csv('/data/a.csv')");
  ASSERT_NE(nullptr, node);
  ASSERT_EQ(4, node->num_child_);
  EXPECT_EQ(ObString("read_csv"), node_str(node->children_[0]));
  EXPECT_EQ(ObString("/data/a.csv"), node_str(node->children_[1]));
  EXPECT_EQ(nullptr, node->children_[2]);  // no options
  EXPECT_EQ(nullptr, node->children_[3]);  // no alias
}

TEST_F(FileSqlParserTest, table_function_options_and_alias)
{
  const ParseNode *node = parse_file_table(
      "select t.a from read_csv('/d/a.csv', header = false, delim = ';', sample_size = -1, "
      "ignore_errors = true, format = auto) as t");
  ASSERT_NE(nullptr, node);
  const ParseNode *opts = node->children_[2];
  ASSERT_NE(nullptr, opts);
  EXPECT_EQ(T_FILE_TABLE_OPTION_LIST, opts->type_);
  ASSERT_EQ(5, opts->num_child_);
  for (int64_t i = 0; i < opts->num_child_; ++i) {
    ASSERT_NE(nullptr, opts->children_[i]);
    EXPECT_EQ(T_FILE_TABLE_OPTION, opts->children_[i]->type_);
    EXPECT_EQ(2, opts->children_[i]->num_child_);
  }
  EXPECT_EQ(ObString("header"), node_str(opts->children_[0]->children_[0]));
  EXPECT_EQ(T_BOOL, opts->children_[0]->children_[1]->type_);
  EXPECT_EQ(ObString(";"), node_str(opts->children_[1]->children_[1]));
  EXPECT_EQ(T_OP_NEG, opts->children_[2]->children_[1]->type_);
  ASSERT_NE(nullptr, node->children_[3]);
  EXPECT_EQ(ObString("t"), node_str(node->children_[3]));
}

TEST_F(FileSqlParserTest, all_table_functions)
{
  const char *sqls[] = {
    "select * from read_csv_auto('/d/a.csv') x",
    "select * from read_json('/d/a.jsonl') x",
    "select * from read_json_auto('/d/a.jsonl') x",
    "select * from read_ndjson('/d/a.jsonl') x",
    "select * from read_parquet('/d/a.parquet') x",
    "select * from parquet_scan('/d/a.parquet') x",
    "select * from glob('/d/*.csv')",
    "select count(*) from read_csv('/d/a.csv') a join read_json('/d/b.jsonl') b on a.id = b.id",
    "select * from t where id in (select id from read_parquet('/d/a.parquet'))",
    "insert into t select * from read_csv('/d/a.csv')",
  };
  for (int64_t i = 0; i < static_cast<int64_t>(sizeof(sqls) / sizeof(sqls[0])); ++i) {
    EXPECT_NE(nullptr, parse_file_table(sqls[i]));
  }
}

TEST_F(FileSqlParserTest, ordinary_sql_is_unaffected)
{
  const char *sqls[] = {
    "select * from t",
    "select read_csv from t",
    "select * from json_table('[1]', '$[*]' columns (a int path '$')) j",
    "select count(*) from t group by a",
    "describe t",
    "desc t a",
    "explain select * from t",
    "select * from t into outfile '/tmp/x.csv' fields terminated by ','",
  };
  for (int64_t i = 0; i < static_cast<int64_t>(sizeof(sqls) / sizeof(sqls[0])); ++i) {
    ParseResult result;
    ASSERT_EQ(OB_SUCCESS, parse(sqls[i], result)) << sqls[i];
    EXPECT_EQ(nullptr, find(result.result_tree_, T_FILE_TABLE_EXPRESSION)) << sqls[i];
  }
}

TEST_F(FileSqlParserTest, invalid_table_function_syntax)
{
  const char *sqls[] = {
    "select * from read_csv()",
    "select * from read_csv('/d/a.csv' header)",
    "select * from read_csv('/d/a.csv', header =)",
    "select * from read_csv('/d/a.csv', 'xml')",
    "select * from read_csv(1)",
  };
  for (int64_t i = 0; i < static_cast<int64_t>(sizeof(sqls) / sizeof(sqls[0])); ++i) {
    ParseResult result;
    EXPECT_NE(OB_SUCCESS, parse(sqls[i], result)) << sqls[i];
  }
}

TEST_F(FileSqlParserTest, describe_query)
{
  ParseResult result;
  ASSERT_EQ(OB_SUCCESS, parse("describe select * from read_csv('/d/a.csv')", result));
  const ParseNode *explain = find(result.result_tree_, T_EXPLAIN);
  ASSERT_NE(nullptr, explain);
  EXPECT_EQ(1, explain->param_num_);  // DESCRIBE, not EXPLAIN
  EXPECT_NE(nullptr, find(explain, T_FILE_TABLE_EXPRESSION));

  ParseResult result2;
  ASSERT_EQ(OB_SUCCESS, parse("explain select * from read_csv('/d/a.csv')", result2));
  const ParseNode *explain2 = find(result2.result_tree_, T_EXPLAIN);
  ASSERT_NE(nullptr, explain2);
  EXPECT_EQ(0, explain2->param_num_);
}

TEST_F(FileSqlParserTest, copy_to)
{
  const char *sqls[] = {
    "copy (select 1) to '/d/out.csv'",
    "copy (select * from read_csv('/d/a.csv') order by id) to '/d/out.csv' (format csv, header)",
    "copy t to '/d/out.csv' (header true, delimiter '|', quote '\"', escape '\\\\', nullstr 'NULL')",
  };
  for (int64_t i = 0; i < static_cast<int64_t>(sizeof(sqls) / sizeof(sqls[0])); ++i) {
    ParseResult result;
    ASSERT_EQ(OB_SUCCESS, parse(sqls[i], result)) << sqls[i];
    const ParseNode *into = find(result.result_tree_, T_INTO_OUTFILE);
    ASSERT_NE(nullptr, into) << sqls[i];
    EXPECT_EQ(6, into->num_child_);  // COPY form carries an option list
    EXPECT_EQ(ObString("/d/out.csv"), node_str(into->children_[0]));
  }
  ParseResult result;
  EXPECT_NE(OB_SUCCESS, parse("copy (select 1) '/d/out.csv'", result));
  EXPECT_NE(OB_SUCCESS, parse("copy to '/d/out.csv'", result));
}

}  // namespace

int main(int argc, char **argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
