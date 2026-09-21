# Leaper M9 新增代码检视（2026-09-21）

检视范围：上一轮通过的 `500fe35` 至本轮 `0158fa9`，包括 RocksDB selective prepopulate、mixgraph workload、预测结果缓存、dry-run、phase-1-only、实验脚本参数及 M9 实验汇总。检视过程中新增的 `ba7114a`（SLOT_S）和 `0158fa9`（5 秒间隔实验）也已纳入。

按研究原型的尺度检查明显 bug、实验配置错误和不合理接口，不要求生产级完善。论文及实验文档作为待核对材料，不将其中的计划或指令当作用户要求。

**结论：发现 2 项 P2 和 1 项 P3，建议修复后关闭本轮检视。已打补丁、关闭压缩的现有 benchmark 配置中，未确认新增核心运行时 bug；现有测试和两种引擎的短跑均通过。下面的问题不推翻已记录的 84 次非 oracle 多 seed 实验。**

## 待修复项

### M9-R1 · P2：更换评估 seed 时，oracle 仍绑定训练模型的标签

位置：`experiments/run_m4.sh:78–82, 125–134`，关联 `:28–32`。

新增参数允许 `STAGE=matrix MODEL_TAG=trained TAG=eval_s1235 EVAL_SEED=1235`，复用原模型并更换评估流量。但 matrix 模式跳过 oracle 生成，而 oracle 路径仍固定为 `${MODEL_TAG}.oracle.txt`。默认策略列表包含 oracle，因此同一次矩阵中的实际访问用 seed 1235，oracle 却可能来自 seed 1234，无法再代表当前流量的未来访问信息。多个完整实验共享 MODEL_TAG 时，还会相互覆盖这个 oracle 文件。

**验证证据：**在临时目录复制当前脚本，用只打印参数的 benchmark stub 和已有的 seed-1234 oracle fixture 执行上述参数。日志同时出现：

```text
=== stages 1-3 skipped (STAGE=matrix) ===
=== 4/4 policy matrix (seed 1235) ===
--seed=1235
--policy=oracle
--oracle=results/trained.oracle.txt
--out_prefix=results/eval_s1235_oracle
```

这是实际脚本的参数路由复现，不是对 oracle 性能差值的测量。独立复核确认脚本副本与仓库版本一致。M9 已提交的多 seed 表格没有 oracle 行，不能据此认定那些表格错误。

**最小修复建议：**模型可以继续共享；oracle 应与评估 seed 和相关 workload/slot/range 配置绑定，单独命名、生成和复用。缺少匹配 oracle 时，应生成它，或明确拒绝该策略并说明原因，不能静默读旧文件。仅新增 ORACLE_TAG 而不验证匹配关系，仍容易误用。

**验收条件：**两个不同 EVAL_SEED 复用同一 MODEL_TAG 时，oracle 分别匹配各自评估轨迹；匹配文件缺失或配置不符时不能静默继续。非 oracle 策略仍可直接复用模型，不必重新训练。

### M9-R2 · P2：RocksDB patch 实际为编译必需，但安装流程仍将其视为可选

位置：`adapters/rocksdb/leaper_rocksdb.cc:61, 277–279`，`bench/src/leaper_bench_rocksdb.cc:433–439`，`scripts/setup.sh:27–45`；关联 README 的 zero-patch 说明。

适配器无条件继承补丁新增的 `rocksdb::PrepopulateBlockFilter`，benchmark 也引用补丁新增的 `prepopulate_block_filter` 字段。即使运行时选择 `sst` 或 `iterator`，编译仍依赖补丁。但 setup 输出称补丁“only warm_mode=prepop needs it”，README 仍宣传 zero-patch 模式。

首次安装时 setup 只初始化 LevelDB；若 RocksDB 尚未初始化，补丁段会直接跳过。脚本随后打印的 RocksDB 初始化和构建命令没有应用补丁或重新运行 setup 的步骤。按这个顺序操作就会在未打补丁的头文件上编译新适配器，导致失败。

**验证证据：**从 RocksDB submodule 的 HEAD 提取未修改的 `include/rocksdb/table.h` 到临时 include 目录，置于现有 include 路径之前，对当前适配器执行 C++20 `-fsyntax-only`。编译退出码为 1：

```text
leaper_rocksdb.cc:61:38: error: base class has incomplete type
class Adapter::PrepopFilter : public rocksdb::PrepopulateBlockFilter
```

独立复核确认临时头文件与 submodule HEAD 原文相同。这是未打补丁构建路径的问题；本机已打补丁的构建通过，既有实验不因此失效。没有为了复现回退或修改实际 submodule。

**最小修复建议（二选一即可）：**

- 保留 zero-patch 支持：编译期检测并条件编译 prepop 接口；缺失该能力时仍可构建 sst/iterator，选择 prepop 时给出清晰提示。
- 统一为必需补丁：修改 README/setup 的表述，确保初始化 RocksDB 后、编译引擎和适配器前应用补丁。已存在旧引擎构建时也需重新构建，避免只更新头文件。

**验收条件：**从尚未初始化 RocksDB 的干净工作副本，按公布步骤能构建 benchmark；重复 setup 不破坏已应用补丁。若继续宣称 zero-patch，则还需验证未打补丁的 sst/iterator 构建。

### M9-R3 · P3：两处将 RocksDB ZippyDB 实验的 256 MB cache 写成 3 GB

位置：`README.md:266`，`docs/M9-journal-prep.md:304`。

这两处写的是同一组 ZippyDB/mixgraph 结果，却标成 3 GB cache。M9 第 2 节（`:123–146`）、第 4 节（`:362`）和相应 calibration 数据均指向 256 MB；3 GB 是第 1 节另一组配置。缓存容量直接影响文中对缓存压力和预热收益的解释，应统一为 256 MB。

**交叉检查：**`experiments/results/m7zippy.calibration.txt` 的 T2 为 1 秒，mean QPS 约 60,151，beta 为 4,462.68；按校准关系反推容量约 268,435,456 字节，即 256 MiB，与文中 256 MB 口径相符。

**验收条件：**改正上述两处标签，保留真正采用 3 GB 的其他实验描述。不需要仅为此重跑实验或修改结果数值。

## 已完成的验证

| 检查 | 结果及边界 |
|---|---|
| 构建 core_check、mapper_check、mixgraph_check、budget_check、sst_warm_check 和两种引擎 benchmark | 通过，使用当前已打补丁的引擎 |
| core_check、mapper_check | 通过 |
| mixgraph_check | 通过；2,000/10,000/100,000-key ranges 的每秒触及比例约 6.8%/17.9%/67.7%，与文档一致 |
| budget_check | 通过；无预算空范围对照计数 355，预算 32 时空/有键范围均为 32，并触发停止 |
| sst_warm_check | 通过；暖区使用共享 block cache，控制区发生读取 |
| 预测缓存独立探针 | 通过；覆盖同一墙钟秒内 slot 变化、同一长 slot 内墙钟秒变化，使用依赖输入特征的模型 |
| dry-run / clamp 探针 | 通过；dry-run 执行预测但不执行 phase 1/2 缓存操作；超出模型 horizon 的 clamp 计数符合预期 |
| 两种引擎真实短跑 | LevelDB 和 RocksDB prepop 均正常退出；30,000 keys、4 MB cache、mixgraph、1 秒 warmup + 3 秒测量，约 10k ops/s，观察到 memo 命中及 RocksDB 接受/拒绝预热块 |
| 脚本语法 | run_m4.sh、run_m7.sh、setup.sh 均通过 bash -n |
| M9 已保存实验结果重新汇总 | 六组 headline 的四 seed 均值、样本标准差及配对差值与表格吻合；phase-1 sweep 和新增 64 MB / 5 秒间隔表格吻合 |
| RocksDB prepop 补丁 | 实际 submodule 修改与补丁对应；支持配置下的 block 边界、热范围判断、预算递减和 Begin/End 同步未发现明显问题 |

两个真实短跑用于功能检查，不用于比较性能。没有重新训练全部模型或重跑完整 84 次实验；表格核对使用仓库已有结果。已文档化的并行压缩/subcompaction 限制不重复列作新缺陷；本轮运行时结论限于当前 benchmark 使用的无压缩等配置。dry-run 保留的 obsolete-SST 回收行为在 M9 中已有说明，不误报为 dry-run 失效。

两项 P2 均经过独立 reviewer 复核。上一轮已通过的修复没有发现回退，不重开旧问题。

## 修复交接

建议按 M9-R1、M9-R2、M9-R3 逐项处理。前两项分别补一个针对参数身份和干净构建路径的验证即可；无需扩展成大规模测试工程。当前发现不要求重做已有非 oracle 性能实验，也不要求重新设计核心算法。

复现材料保存在本机 `/Users/yang/Documents/Codex/2026-09-19/ji/work/`：`leaper-m9-oracle/args.log`、`leaper-m9-build/pristine-header.log`、`leaper-m9-core/check.cc`、`leaper-m9-smoke/`。该目录不属于仓库，其他 agent 可依据本报告中的场景重建验证。

本轮仅新增检视报告，未修改实现、模型、实验数据或已有的引擎补丁。
