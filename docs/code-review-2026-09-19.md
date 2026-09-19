# Leaper 代码检视

检视日期：2026-09-19。检视基线：主仓库 `90fadc4` 与当前工作区；开始时已有 `third_party/leveldb` 修改，检视中未改动仓库。

按研究原型的尺度检查明显 bug、策略实现和实验参数衔接。阅读 README、M8 当前结果、论文第 6 节，以及核心、两种适配器、训练与实验代码。未将文档里的操作文字作为新任务执行；未重跑完整实验矩阵。

总体判断：核心与适配器分层合理，现有验证能确认模型打分和 RocksDB 缓存共享机制有效。但有几处当前代码错误会改变实际运行的策略，建议先修复再确认受影响的结果。以下发现经过独立复核；没有发现或证明数据库记录损坏。

## 优先修复

### 1. P1：LevelDB 嵌套 flush 覆盖外层 compaction 状态

- 位置：[leaper_leveldb.cc:155](../adapters/leveldb/leaper_leveldb.cc:155)、[leaper_leveldb.cc:174](../adapters/leveldb/leaper_leveldb.cc:174)。
- 引擎触发点：[db_impl.cc:1017](../third_party/leveldb/db/db_impl.cc:1017) 在 compaction 中途调用 `CompactMemTable()`，后者触发 flush 的 Begin/Output/End 回调。因此，即使只有一个后台线程，回调仍然会嵌套。
- 适配器只保存一份 `pending_warm_`、`current_output_`；核心也只保存一份热区预测、预算和 flush 标志。内层 flush 覆盖这些状态，结束后未恢复外层状态。
- 最小复现：外层 compaction 两个块，中间插入单块 flush；WarmAll 实际只预取 flush 的一个块，预期三个。Leaper 的外层热区预测也会丢失，使后续预取停止。
- 建议：为后台操作保存独立上下文；LevelDB 可先采用支持嵌套的上下文栈，同时恢复输出文件、待预取列表、热区和预算。

### 2. P2：LevelDB 的 learned 策略不会预取 flush 输出

- 位置：[leaper.cc:239](../leaper/src/leaper.cc:239)，调用来源：[db_impl.cc:587](../third_party/leveldb/db/db_impl.cc:587)。
- flush 传入空的输入块列表，而核心仅从输入块生成候选范围。因此 flush 分支永远预测零个范围，随后所有输出块均被拒绝。
- 最小复现：使用两个永远预测“热”的模型，flush 输出的 `ShouldPrefetch` 仍返回 false。
- 影响：Leaper 与 WarmFlush/WarmAll 实际比较的覆盖范围不同，可能低估 learned 策略。这并不直接证明任何历史收益数字应上调多少。
- 建议：flush 从 memtable 的键范围生成候选，或在输出块出现时预测对应范围。

### 3. P2：重叠区间扫描会遗漏热块，导致 phase 1 误驱逐

- 位置：[overlap.cc:55](../leaper/src/overlap.cc:55)。
- 合并扫描按当前块的结束位置推进热区游标，但来自不同 SST 的输入块可能重叠或嵌套。仅按起点排序不足以支持这个算法。
- 最小复现：块 `[0,10]`、`[1,2]`，热区 `{0,2,4}`，结果为索引 `0,0,0`，应至少包含 `0,1`。重复索引本身会被 keep-mask 吸收，真正的问题是遗漏索引 1，使热块被驱逐。
- 建议：优先对每个块使用已有的二分相交判断，待需要优化时再实现支持重叠块的扫描。
- 影响范围：启用 phase 1 的结果；prefetch-only 配置不走这个驱逐分支。

### 4. P2：RocksDB SST 预取预算在实际循环中不生效

- 位置：[leaper_rocksdb.cc:203](../adapters/rocksdb/leaper_rocksdb.cc:203)，循环位于 214–235 行。
- `WarmFromFiles` 只在入口检查预算，遍历所有文件和热区后才累计实际读块数。因此即使单个任务也可以大幅超预算。
- 实际 SST 小测试：通过公开 listener 回调执行一次预取，适配器配置的预取预算为 1 个块，实际读入 345 个数据块（1 个文件，打开失败数为 0）。测试使用当前核心及适配器源码编译。
- `warm_blocks_this_job_` 还是所有后台任务共享的计数器，每次 Begin 都归零；另一个任务开始会重置正在预取的任务的额度，计数与 job ID 不对应。
- 建议：把预算放入每个 job 的上下文，在预取循环中持续扣减并及时停止，而非全部完成后记账。
- 这说明 M8 中“预算已修复”的描述尚未完整落地。不能据此断言所有已提交运行都发生了超额预取；实际影响需查看输出规模和预算。

## 实验与配置问题

### 5. P2：RocksDB 校准脚本把缓存容量写死为 128 MB

- 位置：[run_m7.sh:55](../experiments/run_m7.sh:55)。
- 工作负载支持 `CACHE_MB`，但校准总是使用 `--cache_mb=128`。论文规模配置为 3072 MB；校准的 beta 在线又除以真实缓存容量，使估计 T2 在相同 QPS 下缩小 24 倍。
- 已提交数据回算：`m7zipf03` 校准恢复时间 8 秒，使用真实缓存容量代入得到 0.3333 秒，最终 `k2=1`；`m7fit_ec/im` 的 2 秒变为 0.0833 秒，也变为一步。
- 建议：校准与工作负载使用同一缓存容量变量，再重跑受影响的预测策略。
- 24 倍指时间估计公式的偏差，不是性能偏差；在线 QPS、取整和模型步数上限也会影响最终窗口。

### 6. P2：训练与在线预测的时间特征仍不一致

- 位置：[train_leaper.py:241](../tools/train_leaper.py:241)、[leaper_bench_rocksdb.cc:449](../bench/src/leaper_bench_rocksdb.cc:449)，对照 [predictor.cc:61](../leaper/src/predictor.cc:61)。
- 训练历史为 `s-6..s-1`，step 1 标签为 `s`，时间特征却写为 `s+1`；在线对相同完整历史使用当前时间 `s`。
- RocksDB trace meta 还遗漏 `clock_offset_s`，训练默认偏移零，而在线时钟包含 30 秒 warmup。
- 真实调用特征构造函数验证：trace slot 6、历史 `[0,1,2,3,4,5]`，在线对应第 36 秒；LevelDB 训练特征为 37 秒，RocksDB 为 7 秒。
- 建议：统一预测时刻和历史窗口约定，给 RocksDB meta 补齐时钟偏移。影响大小取决于模型是否使用时间特征，不能仅凭错位推算命中率变化。

### 7. P2：单模型配置会让 compaction 预取为空

- 位置：[predictor.cc:87](../leaper/src/predictor.cc:87)，对照 [leaper.cc:255](../leaper/src/leaper.cc:255)。
- 接口承诺单模型会被复用到所有步长，LevelDB bench 默认也只有一步；实际预测函数把上界截为模型数 1，而 compaction 的 phase 2 下界至少为 2，直接得到空结果。
- 最小复现：一个永远预测“热”的模型，正常 compaction 的 `ShouldPrefetch` 仍为 false。
- 建议：实现承诺的单模型复用，或明确拒绝不支持的配置并修正文档与默认值。
- 六步模型矩阵不直接受这个“单模型”问题影响，不需要因此把整套既有结果作废。

## 验证与判断边界

已运行并通过仓库现有 `mapper_check`、`gbdt_check` 和 `sst_warm_check`。模型打分核对 2,000 行，最大误差约 `2.96e-8`；SST 测试证明预取与 DB Get 使用相同缓存键，热区不再读盘而冷区仍读盘。

嵌套 flush、空候选、区间遗漏和单模型问题使用当前源码编译的独立小程序复现。训练时间特征使用真实 Python 构造函数核对，T2 使用已提交校准文件和 CSV 回算。未执行完整 benchmark，因而不预测修复后的收益排名。

设计层面，不建议大改 core/adapter 分层。优先补齐每个后台操作的状态隔离、候选范围和预算语义。RocksDB 需要重读输出文件、缺少 phase 1、合成负载不等于真实论文负载等已公开说明的限制，不作为本次新增 bug。若以后追求更公平的性能比较，再考虑把选择逻辑放进 RocksDB 的原生预填充路径。
