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
// Parser tests for AI COLUMN definitions (CREATE / ADD / MODIFY), SHOW AI COLUMNS and
// RETRY AI COLUMN, plus the text helpers that turn a definition into the worker expression.

#include <gtest/gtest.h>
#include "sql/parser/ob_parser.h"
#include "sql/parser/parse_node.h"
#include "share/ai_service/ob_ai_service_struct.h"

using namespace oceanbase::common;
using namespace oceanbase::sql;
using namespace oceanbase::share;

namespace
{

class AiColumnParserTest : public ::testing::Test
{
protected:
  int parse(const char *sql, ParseResult &result)
  {
    ObParser parser(allocator_, SMO_DEFAULT);
    memset(&result, 0, sizeof(result));
    return parser.parse(ObString(sql), result);
  }
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
    return nullptr == node ? ObString()
                           : ObString(static_cast<int32_t>(node->str_len_), node->str_value_);
  }
  const ParseNode *parse_and_find(const char *sql, const ObItemType type)
  {
    ParseResult result;
    EXPECT_EQ(OB_SUCCESS, parse(sql, result)) << sql;
    const ParseNode *node = find(result.result_tree_, type);
    EXPECT_NE(nullptr, node) << sql;
    return node;
  }
  ObArenaAllocator allocator_;
};

TEST_F(AiColumnParserTest, create_table_ai_column)
{
  const ParseNode *node = parse_and_find(
      "create table t(id bigint primary key, content varchar(100), "
      "tags json AI COLUMN (AI_COMPLETE('m', concat('Classify: ', content))) "
      "WITH (sync_mode='async', max_retries=2))",
      T_AI_COLUMN_DEFINITION);
  ASSERT_NE(nullptr, node);
  ASSERT_EQ(2, node->num_child_);
  EXPECT_EQ(ObString("AI_COMPLETE('m', concat('Classify: ', content))"),
            node_str(node->children_[0]));
  EXPECT_NE(nullptr, node->children_[1]);  // WITH options
}

TEST_F(AiColumnParserTest, create_table_ai_column_without_options)
{
  const ParseNode *node = parse_and_find(
      "create table t(c varchar(10), e vector(4) AI COLUMN (AI_EMBED('m', c, 4)))",
      T_AI_COLUMN_DEFINITION);
  ASSERT_NE(nullptr, node);
  EXPECT_EQ(ObString("AI_EMBED('m', c, 4)"), node_str(node->children_[0]));
  EXPECT_EQ(nullptr, node->children_[1]);
}

TEST_F(AiColumnParserTest, alter_add_and_modify)
{
  EXPECT_NE(nullptr, parse_and_find("alter table t add column r varchar(100) AI COLUMN "
                                    "(AI_COMPLETE('m', c)) WITH (max_retries=1)",
                                    T_AI_COLUMN_DEFINITION));
  // MODIFY parses; the resolver rejects it
  EXPECT_NE(nullptr, parse_and_find("alter table t modify column r varchar(100) AI COLUMN "
                                    "(AI_COMPLETE('m', c))",
                                    T_AI_COLUMN_DEFINITION));
}

TEST_F(AiColumnParserTest, show_ai_columns)
{
  const ParseNode *node = parse_and_find("show ai columns from t from db", T_SHOW_AI_COLUMNS);
  ASSERT_NE(nullptr, node);
  EXPECT_EQ(2, node->num_child_);
  EXPECT_NE(nullptr, parse_and_find("show ai columns in t", T_SHOW_AI_COLUMNS));
}

TEST_F(AiColumnParserTest, retry_ai_column)
{
  const ParseNode *node = parse_and_find("alter table t retry ai column result", T_AI_COLUMN_RETRY);
  ASSERT_NE(nullptr, node);
  EXPECT_EQ(2, node->num_child_);
  node = parse_and_find("alter table t retry ai column result where status = 'FAILED' and id in (1, 3)",
                        T_AI_COLUMN_RETRY);
  ASSERT_NE(nullptr, node);
  ASSERT_EQ(3, node->num_child_);
  EXPECT_EQ(ObString("status = 'FAILED' and id in (1, 3)"), node_str(node->children_[2]));
}

TEST(AiColumnUtilTest, split_call_args_nested)
{
  ObArenaAllocator allocator;
  ObString name;
  ObSEArray<ObString, 4> args;
  ASSERT_EQ(OB_SUCCESS, AiColumnUtil::split_call_args(
      ObString("AI_COMPLETE('m', concat('a, b', f(x, y)), '{\"k\": \"(\"}')"), name, args));
  EXPECT_EQ(ObString("AI_COMPLETE"), name);
  ASSERT_EQ(3, args.count());
  EXPECT_EQ(ObString("'m'"), args.at(0));
  EXPECT_EQ(ObString("concat('a, b', f(x, y))"), args.at(1));
  EXPECT_EQ(ObString("'{\"k\": \"(\"}'"), args.at(2));
  EXPECT_NE(OB_SUCCESS, AiColumnUtil::split_call_args(ObString("AI_COMPLETE('m', c"), name, args));
  EXPECT_NE(OB_SUCCESS, AiColumnUtil::split_call_args(ObString("AI_COMPLETE('m', c) + 1"), name, args));
}

TEST(AiColumnUtilTest, normalized_expr_carries_type_prompt)
{
  ObArenaAllocator allocator;
  ObSEArray<ObString, 4> args;
  ObString expr;
  ASSERT_EQ(OB_SUCCESS, args.push_back(ObString("'m'")));
  ASSERT_EQ(OB_SUCCESS, args.push_back(ObString("content")));
  ASSERT_EQ(OB_SUCCESS, AiColumnUtil::build_normalized_expr(
      allocator, ObString(AiColumnUtil::FUNC_AI_COMPLETE), args, ObJsonType, expr));
  EXPECT_NE(nullptr, strstr(std::string(expr.ptr(), expr.length()).c_str(), "valid JSON value"));
  // user options win over the generated system prompt
  ASSERT_EQ(OB_SUCCESS, args.push_back(ObString("'{\"temperature\": 0}'")));
  ASSERT_EQ(OB_SUCCESS, AiColumnUtil::build_normalized_expr(
      allocator, ObString(AiColumnUtil::FUNC_AI_COMPLETE), args, ObIntType, expr));
  std::string s(expr.ptr(), expr.length());
  EXPECT_NE(std::string::npos, s.find("JSON_MERGE_PATCH("));
  EXPECT_NE(std::string::npos, s.find("base-10 integer"));
  // embeddings take no prompt
  args.reset();
  ASSERT_EQ(OB_SUCCESS, args.push_back(ObString("'m'")));
  ASSERT_EQ(OB_SUCCESS, args.push_back(ObString("c")));
  ASSERT_EQ(OB_SUCCESS, AiColumnUtil::build_normalized_expr(
      allocator, ObString(AiColumnUtil::FUNC_AI_EMBED), args, ObCollectionSQLType, expr));
  EXPECT_EQ(ObString("AI_EMBED('m', c)"), expr);
}

TEST(AiColumnUtilTest, status_pseudo_column)
{
  ObArenaAllocator allocator;
  ObString out;
  ASSERT_EQ(OB_SUCCESS, AiColumnUtil::replace_status_pseudo_column(
      allocator, ObString("status = 'FAILED' and `status` = 'status' and t.status = 1 and my_status = 2"),
      ObString("S"), out));
  EXPECT_EQ(ObString("S = 'FAILED' and `status` = 'status' and t.status = 1 and my_status = 2"), out);
  ASSERT_EQ(OB_SUCCESS, AiColumnUtil::replace_status_pseudo_column(
      allocator, ObString("STATUS='READY'"), ObString("S"), out));
  EXPECT_EQ(ObString("S='READY'"), out);
}

TEST(AiColumnUtilTest, hidden_column_names)
{
  ObArenaAllocator allocator;
  ObString name;
  ASSERT_EQ(OB_SUCCESS, AiColumnUtil::gen_hidden_column_name(
      allocator, ObString("tags"), AiColumnUtil::HIDDEN_STATE, name));
  EXPECT_EQ(ObString("__ai_tags_state"), name);
  EXPECT_TRUE(AiColumnUtil::is_hidden_column_name(name));
  EXPECT_FALSE(AiColumnUtil::is_hidden_column_name(ObString("tags")));
}

}  // namespace

int main(int argc, char **argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
