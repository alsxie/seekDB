# 赛题四：全文索引调优说明

作者：FDE WorkMate

基线：`5bce9e06d`（未修改的 2026-competition）。测试机 ss144，`tools/benchmark/fts_large_bench.sh`，基线与优化版交替各跑 3 轮，按官方评分公式（三类指标等权）计算。

## 结果

| 指标 | 基线 mean±sd | 优化后 mean±sd | 下降 |
|---|---|---|---|
| build_ik_all_sec | 8.225±0.048 | 3.883±0.048 | 52.8% |
| build_ik_content_sec | 6.400±0.094 | 2.826±0.001 | 55.8% |
| build_beng_en_sec | 3.059±0.047 | 1.455±0.047 | 52.4% |
| tokenize_ik_avg_ms | 0.0878±0.0008 | 0.0667±0.0014 | 24.0% |
| tokenize_beng_avg_ms | 0.0407±0.0011 | 0.0365±0.0009 | 10.2% |
| query_cn_avg_ms | 1.948±0.010 | 1.132±0.036 | 41.9% |
| query_beng_avg_ms | 2.840±0.013 | 1.590±0.003 | 44.0% |
| query_mixed_avg_ms | 2.117±0.009 | 1.214±0.008 | 42.7% |
| query_limit_avg_ms | 2.379±0.021 | 0.180±0.004 | 92.4% |

分类平均：构建约 54%，分词约 17%，查询约 55%，综合约 42%。

正确性：67089 行 TOKENIZE/MATCH dump（分词结果、命中 id、排序、BM25 分数）与基线逐字节一致；fts_index、ai_function、file_sql、ai_column 四个官方用例集全部通过，仅 `basic_dml`、`partitioned_simple_query` 两处与基线版本相同的已知差异（plan 参数编号、内部表 id）。

## 瓶颈依据与修改点

### 构建

- 瓶颈：perf 显示无显式并行度时任务 DOP=1，单个 PX worker 串行完成分词、排序、写入；word_doc / doc_word 倒排表占构建时间的大头。
- 修改：`ObFtsIndexBuilderUtil::decide_inverted_build_parallelism`，只对倒排表（FTS_INDEX、FTS_DOC_WORD）在未指定并行度时取 `MIN(4, cpu/2)`。doc id 表保持上游并行度，否则 doc id 分配顺序随运行变化，导致无 ORDER BY 的 LIMIT 并列结果不稳定。
- utf8mb4_general_ci 比较跳过相同的前缀（`ob_ctype_utf8.cc`），`ObCharset::get_charset` 内联快路径：排序键多为长公共前缀的词。
- 分词哈希表使用 `NoPthreadDefendMode`（单次调用独占，无需桶锁）。

### 分词（TOKENIZE）

- 瓶颈：每次调用重复解析 parser 属性 JSON、加锁查词典 loader、范围词典线性查找、按 `ObFTSingleWord` 拷贝 key 查哈希。
- 修改：
  - 属性规范化结果按线程记忆（`ob_expr_tokenize.cpp`、`ob_fts_parser_property.cpp`），输入相同则直接复用；
  - 词典 loader 查找改为读锁快路径（`ob_gen_dic_loader.cpp`）；
  - 范围词典有序且不重叠时二分查找，重叠时保持原线性首个匹配语义（`ob_ft_range_dict.cpp`）；
  - DAT 字符码查找直接用 key 字节探测（`ob_ft_dat_dict.cpp`）；
  - IK 仲裁器 chain map 去掉线程锁。
- 返回类型仍为 JSON，分词规则不变。

### 查询（MATCH）

- 只用作过滤条件的 MATCH：自然语言模式下 BM25 分数在命中时恒 > 0，过滤结果等价于“是否命中”，跳过相关度计算（`relevance_only_for_match_filter_`）。
- 无分数、无投影、无过滤且任一维命中即有效时，倒排列表合并改为对 ≤8 个维度头做线性最小值扫描（unscored merge，`ob_sparse_daat_iter.cpp`），绕过堆和相关度 collector。
- unscored merge 按批填充（`fill_unscored_batch`）：一次调用产出整批结果，裸指针访问维度头，id 比较内联，offset/limit 在原处处理，输出顺序与堆合并相同。
- doc id（varbinary）比较内联，按 8 字节大端字比较（结果与 memcmp 序相同），避免逐次走 collation 函数表（`ob_sparse_retrieval_util.h`）。
- 倒排扫描保存 doc id 时每批只检查一次 skip 向量（`ob_text_retrieval_token_iter.cpp`）；结果投影在向量化且表达式为批结果时整批定位 datum、整段置 evaluated 标志（`project_results`）。
- string diff 编码的定长列解码快路径（`ob_string_diff_decoder.cpp`）：公共字节先拼成模板，每行一次模板拷贝加各差异段，用定长搬移代替逐段 libc memcpy；布局超出限制（串长 >32、差异段 >8、描述符未覆盖全串、hex packing、变长）时走原路径。用双路径自校验构建（快路径与原路径逐行比较，不一致即中止）跑完用例集、模糊测试和 benchmark，共校验 2782 万行，0 不一致；自校验补丁不随代码提交。
- 有 LIMIT 下推且只需“是否命中”时保持 DaaT 流式，命中够数即停（`ob_das_iter_utils.cpp`）。
- 父查询不读取列的 LIMIT 视图（如 `select count(*) from (... match ... limit k) v`）：不再追加隐式相关度排序，并裁剪无用投影，避免回表（`ob_transform_pre_process.cpp`）。

## 回归用例

`tools/deploy/mysql_test/test_suite/fts_index/t/fts_perf_regression.test` 覆盖以上每一项：并行构建、过滤式 MATCH 与带分数形式结果一致、unscored merge（含 >8 维回退和不存在的词）、并列 LIMIT 顺序、LIMIT 视图、属性记忆、IK 各模式、general_ci 比较。该用例在基线二进制和优化版上输出逐字节一致。

## 已评估未采用

- 编码排序键（DDL 排序在存储层，改动风险高）；
- 跨调用复用 IK 解析器/词典句柄（KV cache 常驻 pin 与词典失效风险）；
- DAT 字符码单字符记忆、groupby 单次哈希探测：实测收益不超过噪声，已回退。

## 注意

构建收益依赖可用核数（自适应并行度 `MIN(4, cpu/2)`）；核数较少的评测机上构建项收益会低于本地数据。
