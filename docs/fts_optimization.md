# 赛题四：全文索引调优说明

作者：FDE WorkMate

基线：`5bce9e06d`（未修改的 2026-competition）。评分按官方公式：三类指标（构建、分词、查询）各自取平均下降，再等权平均为综合下降，综合 50% 为满分。

## 测试条件

- 工具：`tools/benchmark/fts_large_bench.sh`，每次全新实例；基线与优化版在同一形态下交替各跑 3 轮，按同一会话的基线均值计算下降。
- 评测机比测试机慢：用 CI 基线 json 除以本地基线，构建项约 4.2 倍、单语句项约 8~9 倍，说明评测机核数少且受限。因此在 ss144 上仿真两种 CPU 形态：
  - A4（虚机型）：`taskset -c 0-3`，并通过 `LD_PRELOAD` 垫片让 `sysconf` 返回 4 核；
  - B2（容器型）：`docker update --cpus=2`，CFS 配额 2 核，但可见 8 核。
- 形态校准：两次 CI 实测分别为 4d87a0bd 30.70、f0244d75 40.38；对应仿真为 B2 32.3 / A4 40.3。评测机形态不能唯一确定，所以两种形态都要不退化。

## 结果（最终版本，2026-10-29 校准）

A4：

| 指标 | 基线 mean±sd | 优化后 mean±sd | 下降 |
|---|---|---|---|
| build_ik_all_sec | 8.561±0.001 | 4.219±0.236 | 50.7% |
| build_ik_content_sec | 6.433±0.000 | 3.159±0.309 | 50.9% |
| build_beng_en_sec | 3.159±0.047 | 1.590±0.094 | 49.7% |
| tokenize_ik_avg_ms | 0.0951±0.0021 | 0.0731±0.0006 | 23.1% |
| tokenize_beng_avg_ms | 0.0474±0.0004 | 0.0406±0.0009 | 14.3% |
| query_cn_avg_ms | 1.947±0.004 | 0.566±0.008 | 70.9% |
| query_beng_avg_ms | 2.852±0.007 | 0.760±0.006 | 73.4% |
| query_mixed_avg_ms | 2.109±0.003 | 0.644±0.008 | 69.5% |
| query_limit_avg_ms | 2.361±0.004 | 0.189±0.007 | 92.0% |

分类平均：构建 50.4%，分词 18.7%，查询 76.4%，综合 48.5%。

B2：

| 指标 | 基线 mean±sd | 优化后 mean±sd | 下降 |
|---|---|---|---|
| build_ik_all_sec | 8.400±0.050 | 5.340±0.059 | 36.4% |
| build_ik_content_sec | 6.334±0.001 | 3.460±0.046 | 45.4% |
| build_beng_en_sec | 3.093±0.095 | 1.798±0.054 | 41.9% |
| tokenize_ik_avg_ms | 0.0925±0.0085 | 0.0678±0.0011 | 26.7% |
| tokenize_beng_avg_ms | 0.0439±0.0038 | 0.0369±0.0011 | 15.9% |
| query_cn_avg_ms | 1.950±0.007 | 0.555±0.003 | 71.5% |
| query_beng_avg_ms | 2.854±0.013 | 0.747±0.003 | 73.8% |
| query_mixed_avg_ms | 2.114±0.002 | 0.621±0.001 | 70.6% |
| query_limit_avg_ms | 2.350±0.009 | 0.174±0.008 | 92.6% |

分类平均：构建 41.2%，分词 21.3%，查询 77.1%，综合 46.6%。

## 逐版本归因

综合下降（%），每格为同一会话 3~5 轮的均值；不同会话的基线均值不同，跨行比较约有 ±1.5 的噪声。

| 版本 | 主要内容 | A4 | B2 | CI 实测 |
|---|---|---|---|---|
| 4d87a0bd | 分词快路径、过滤式 MATCH、LIMIT 视图、自适应构建并行度 | 35.5 | 32.3 | 30.70 |
| f0244d75 | unscored merge、倒排表专用构建并行度（doc id 确定） | 40.3 | 36.6 | 40.38 |
| 475d8cca | 批量 unscored merge、批量投影、string diff 定长解码 | 46.3 | 41.8 | – |
| 本次 ① | 构建采样修复 + domain id 点查、排序比较裸指针 | 48.3 | 43.9 | – |
| 本次 ② | 构建并行度按可用 CPU（配额/亲和性）计算 | 48.5 | 46.6 | – |

① 中的采样修复使 A4 构建项由 45.5% 升到 53.0%（同会话对照 475d8cca）；② 只改变受配额限制的形态，A4 持平，B2 构建项由 33.8% 升到 41.2%。

## 瓶颈依据与修改点

### 构建

- 并行度：无显式并行度时任务 DOP=1，单个 PX worker 串行完成分词、排序、写入。`ObFtsIndexBuilderUtil::decide_inverted_build_parallelism` 只对倒排表（FTS_INDEX、FTS_DOC_WORD）在未指定并行度时取 `MIN(4, 可用 CPU/2)`；两张倒排表同时构建，各分一半。doc id 表保持上游并行度，否则 doc id 分配顺序随运行变化，无 ORDER BY 的 LIMIT 并列结果不稳定。
- 可用 CPU（本次 ②）：原来按 `get_cpu_num()`（主机在线核数）计算，容器配额 2 核、可见 8 核时取到 4 个 worker，超订后只增加切换和缓存缺失。B2 下并行度对照（同会话）：4 / 2 / 1 个 worker 综合 43.3 / 44.4 / 45.2。新增 `get_available_cpu_num()`（`lib/utility/ob_cpu_limit.h`）：取在线核数、进程 CPU 亲和性、cgroup CPU 配额（v1 `cpu.cfs_quota_us/cpu.cfs_period_us`，v2 `cpu.max`，沿层级取最小值，向上取整）三者最小值，每次调用实时读取。cgroup 读取复用 `get_cgroup_memory_limit` 的挂载点与层级解析，改为按控制器和读取函数参数化；内存限额结果不变（合成 cgroup 树对比改动前后代码，v1/v2、嵌套、无限制各用例一致）。`get_cpu_num()` 本身不变，线程池与内存分配不受影响。服务端日志 `decide inverted table build parallelism` 记录每次的取值。
- 采样修复（本次 ①）：PX 先对源表采样来切分各 worker 的范围。倒排表构建的采样扫描走的是未分词的源行，没有 word 和 doc id 值，切出的边界都落在全部真实 key 之前，于是一个 worker 拿到整张倒排表，其他 worker 空转（线程时间线可见单线程独占）。修改 `ObTableScanOp::inner_get_next_row`：FTS DDL 扫描在采样时同样输出分词后的索引行。采样行的 doc id 由 `ObDASDomainIdMergeIter` 对未采样的 rowkey→doc id 表做点查取得（`lookup_sampled_rows_`），原来两张表各自独立采样后逐行合并，行对不上；点查模式下扫描结束不再排空 rowkey 迭代器，个别取不到 doc id 的采样行填空值，只影响切分估计。
- 排序比较：排序算子 `ObSortOpImpl::Compare`（构建计划中的排序）改为直接访问比较函数与排序列数组（`ObIArray::at()` 带断言，无法内联）。perf 显示 `ObNullSafeDatumStrCmp::cmp` 88% 的自身时间在读取 datum 长度/标志（StoredRow 分散导致缓存缺失），比较器层面的优化上限约为构建 CPU 的 5%，因此只做这一处。
- utf8mb4_general_ci 比较跳过相同前缀（`ob_ctype_utf8.cc`），`ObCharset::get_charset` 内联快路径：排序键多为长公共前缀的词。
- 词频表 `ObFTWordMap`（`data_plane/fts/ob_fts_struct.h`）改用 `NoPthreadDefendMode`：每次分词调用独占一张表，无需桶锁；构建与 TOKENIZE 共用这张表。`groupby_word` 仍是查询加写入两次哈希探测，改成单次探测实测无收益，已回退。

### 分词（TOKENIZE）

- 瓶颈：每次调用重复解析 parser 属性 JSON、加锁查词典 loader、范围词典线性查找、按 `ObFTSingleWord` 拷贝 key 查哈希。
- 修改：
  - 属性规范化结果按线程记忆（`ob_expr_tokenize.cpp`、`ob_fts_parser_property.cpp`），输入相同则直接复用；
  - 词典 loader 查找改为读锁快路径（`ob_gen_dic_loader.cpp`）；
  - 范围词典有序且不重叠时二分查找，重叠时保持原线性首个匹配语义（`ob_ft_range_dict.cpp`）；
  - DAT 字符码查找直接用 key 字节探测（`ob_ft_dat_dict.cpp`）；
  - IK 仲裁器 chain map 去掉线程锁。
- 剩余空间小：beng 分词 `eval_tokenize` 只占进程采样的 14.3%，`ObMPQuery::process` 48.7%、sql-nio 25%，每条语句服务端约 33 µs、端到端约 67 µs，固定的语句开销占主导；IK 的 DAT 查找约占 10.5%，每项候选优化收益不超过 1 个综合点。
- 返回类型仍为 JSON，分词规则不变；未做结果缓存（会变成针对测试数据的优化）。

### 查询（MATCH）

- 只用作过滤条件的 MATCH：自然语言模式下 BM25 分数在命中时恒 > 0，过滤结果等价于“是否命中”，跳过相关度计算（`relevance_only_for_match_filter_`）。
- 无分数、无投影、无过滤且任一维命中即有效时，倒排列表合并改为对 ≤8 个维度头做线性最小值扫描（unscored merge，`ob_sparse_daat_iter.cpp`），绕过堆和相关度 collector。
- unscored merge 按批填充（`fill_unscored_batch`）：一次调用产出整批结果，裸指针访问维度头，id 比较内联，offset/limit 在原处处理，输出顺序与堆合并相同。改前合并约占 query_cn 采样的 43%（每条 posting 约 41 ns）。
- doc id（varbinary）比较内联，按 8 字节大端字比较（结果与 memcmp 序相同），避免逐次走 collation 函数表（`ob_sparse_retrieval_util.h`）。
- 倒排扫描保存 doc id 时每批只检查一次 skip 向量（`ob_text_retrieval_token_iter.cpp`）；结果投影在向量化且表达式为批结果时整批定位 datum、整段置 evaluated 标志（`project_results`）。
- string diff 编码的定长列解码快路径（`ob_string_diff_decoder.cpp`，改前连同 memmove 约占 22%）：公共字节先拼成模板，每行一次模板拷贝加各差异段，用定长搬移代替逐段 libc memcpy；布局超出限制（串长 >32、差异段 >8、描述符未覆盖全串、hex packing、变长）时走原路径。用双路径自校验构建（快路径与原路径逐行比较，不一致即中止）跑完用例集、模糊测试和 benchmark，共校验 2782 万行，0 不一致；自校验补丁不随代码提交。
- 有 LIMIT 下推且只需“是否命中”时保持 DaaT 流式，命中够数即停（`ob_das_iter_utils.cpp`）。
- 父查询不读取列的 LIMIT 视图（如 `select count(*) from (... match ... limit k) v`）：不再追加隐式相关度排序，并裁剪无用投影，避免回表（改前回表 `ObDASCacheLookupIter` 占 68% CPU，query_limit 由 3.2 ms 降到 0.18 ms，`ob_transform_pre_process.cpp`）。

## 正确性

- 67090 行 TOKENIZE/MATCH dump（分词结果、命中 id、排序、BM25 分数）与基线逐字节一致；最终版本在全容器形态（3 个 worker）与 A4（2 个 worker）下的 dump 也逐字节一致。
- 差分模糊测试（collation 比较、TOKENIZE、MATCH 各模式、预编译语句、8 会话并发复现）与上一版本输出一致；与基线相比只有基线自身两次运行间也存在的 BM25 浮点漂移和并列重排。
- 官方用例集 fts_index、vector_index、ai_function、file_sql、ai_column、ddl、table_redefinition：判定为 mysqltest 退出码 0、生成 .out 且与 .result 一致。失败项与基线二进制完全相同，输出逐字节一致（`basic_dml`、`partitioned_simple_query`、`vector_ivf_index_with_filter`、`dump_ddl_mem`，基线本身即不通过，为 plan 参数编号、内部表 id 等环境差异）。

## 回归用例

`tools/deploy/mysql_test/test_suite/fts_index/t/fts_perf_regression.test` 覆盖：并行构建、过滤式 MATCH 与带分数形式结果一致、unscored merge（含 >8 维回退和不存在的词）、并列 LIMIT 顺序、LIMIT 视图、属性记忆、IK 各模式、general_ci 比较。该用例在基线二进制和优化版上输出逐字节一致。

## 已评估未采用

- 编码排序键（DDL 排序在存储层，改动风险高）；
- 跨调用复用 IK 解析器/词典句柄（KV cache 常驻 pin 与词典失效风险）；
- DAT 字符码单字符记忆、groupby 单次哈希探测：实测收益不超过噪声，已回退；
- 修改全局参数默认值（`px_task_size`、工作区比例）与全局 `get_cpu_num()`：影响面超出全文索引；
- DDL 临时数据压缩、FTS DDL 扫描向量化：影响所有 DDL，收益未经验证。

## 注意

构建收益取决于可用 CPU：并行度为 `MIN(4, 可用 CPU/2)`。只有 2 核可用时每张倒排表 1 个 worker（两张表同时构建），构建项收益低于多核形态。
