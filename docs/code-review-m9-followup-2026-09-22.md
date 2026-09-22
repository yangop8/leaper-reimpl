# Leaper M9 修复复核（2026-09-22）

复核提交：`3a807de`，主要修复提交为 `7f93891`；对照 `docs/code-review-m9-2026-09-21.md`。本轮同时检查新加入的 M10 实验链与修复相关的调用路径，继续按研究原型尺度检视。

**结论：上一轮 M9-R1、M9-R3 可以关闭；M9-R2 的主要编译问题已修复，但补丁状态变化后的 CMake 缓存处理还缺一步。此外发现两个确定的实验脚本问题。共剩 3 个 P2 待处理，尚不能全部验收通过。**

当前已打补丁配置的构建、回归测试和 RocksDB prepop 短跑通过。以下发现不推翻已有非 oracle 的 M9 实验表格；无需因此重新训练全部模型或重跑完整实验矩阵。

## 原问题的关闭情况

| 原编号 | 状态 | 复核结果 |
|---|---|---|
| M9-R1：不同评估 seed 误用同一 oracle | 关闭 | oracle 改为 `${TAG}.oracle.txt`，meta 包含 seed、range、slot、warmup 和 workload。缺失时生成，不匹配或缺 meta 时退出 3；非 oracle 策略仍共享模型。独立探针实际生成小型二进制 trace 并调用真实 make_oracle，验证生成、复用、拒绝及新 TAG 分离 |
| M9-R2：补丁实际必需但宣称可选 | 部分关闭 | 适配器和 benchmark 新增条件编译，未打补丁的两个翻译单元均通过语法编译；setup 补充初始化后应用补丁、重建引擎的步骤。已有构建目录切换补丁状态时仍有下面的 F1 |
| M9-R3：两处缓存容量标错 | 关闭 | README 和 M9 regime 表均已改为 256 MB，其他真正使用 3 GB 的配置保留 |

## 剩余问题与修复验收

### F1 · P2：应用 RocksDB 补丁后，普通重新配置仍沿用“无补丁”的检测缓存

位置：`CMakeLists.txt:64–81`，关联 `scripts/setup.sh:27–31`。

`check_cxx_source_compiles` 将 `LEAPER_HAVE_PREPOP_FILTER` 存进 CMake cache。先在未打补丁的 RocksDB 上配置，再应用补丁、重建引擎并按 setup 提示重新配置同一个 build 目录时，这个检测不会自动重跑。适配器仍按无补丁分支编译，`warm_mode=prepop` 被拒绝。反向切换头文件时也可能保留旧的 true 而导致编译失败。

**实际验证：**将当前 CMake 检测段原样提取到隔离的小工程，仅参数化 include 路径；首次使用从 submodule HEAD 提取的 pristine table.h，第二次换回已打补丁头文件，保持同一构建目录：

```text
pristine:             Performing Test ... - Failed; result 为空
patched-reconfigure: 不再出现 Performing Test; result 仍为空
patched-fresh:       Performing Test ... - Success; result=1
```

这个复现没有改动实际引擎或主 build 目录，证明的是检测缓存失效，不是当前已打补丁构建无法运行。

**最小修复：**配置时使能力检测随头文件/补丁状态重新计算，例如清理该检测变量及其 cache 后重测，或以相关头文件内容作为检测缓存身份。若选择只提供人工清缓存步骤，则必须将它明确加入 setup 的升级流程，普通“重新配置”不足以恢复能力。

**验收：**同一构建目录先检测无补丁，再切换到已打补丁头文件，应重新得到 true；新编译的适配器能启用 prepop。最好同时覆盖反向切换。无需重新构建两个完整引擎来验证检测缓存本身。

### F2 · P2：M10 重复实验把显式空的模型后缀替换成结果后缀

位置：`experiments/chains/rocksdb_configs.sh:14, 18`，关联 `docs/M10-dedicated-machine.md` 的 T1、T2 命令。

文档先训练无后缀模型（例如 `m7paper3`），随后以 `STAGE=matrix MODEL_TAG_SUFFIX=""` 和 repeat.sh 运行重复实验，意图将结果写到 `_r1`、`_r2`、`_r3`，继续复用原模型。但 `MSUF=${MODEL_TAG_SUFFIX:-$SUF}` 的 `:-` 会把显式空字符串也视为缺省，于是 MODEL_TAG 变成尚未训练的 `m7paper3_r1`。run_m7.sh 随后读取不存在的 calibration 文件并退出，T1/T2 的公布命令不能完成。

**实际验证：**临时复制真实 repeat.sh 和 rocksdb_configs.sh，仅将末端 run_m7.sh 替换为打印环境变量的 stub，使用文档中的空后缀参数：

```text
TAG=m7paper3_r1 MODEL_TAG=m7paper3_r1 STAGE=matrix
TAG=m7zipf09_r1 MODEL_TAG=m7zipf09_r1 STAGE=matrix
```

第二、三轮同样把 MODEL_TAG 变成 `_r2`、`_r3`。这是参数传递验证，没有启动大型实验。

**最小修复：**区分变量“未设置”和“显式设置为空”，例如改为 `MSUF=${MODEL_TAG_SUFFIX-$SUF}`，保留默认训练时模型与输出同后缀的行为。

**验收：**文档 T1 的三轮结果 TAG 各异，但 MODEL_TAG 始终为 `m7paper3` / `m7zipf09`；T2 的 `_unth_rN` 输出也应复用无后缀模型。另检查 MODEL_TAG_SUFFIX 为非空值及未设置时的原有行为不发生回归。

### F3 · P2：非 1 秒 slot 的 oracle 暖机偏移仍按秒数当作槽数传入

位置：`experiments/run_m4.sh:70–71`，`tools/make_oracle.py:29–31, 57`。

脚本传入 `--slot_s=$SLOT --slot_offset=$WARMUP`，其中 WARMUP 为 30 秒；但工具明确以 interval 数解释 slot_offset，计算的是 `floor(trace_ms / slot_ms) + slot_offset`。在已有的 `SLOT_S=5` 配置中，暖机偏移应为 6 槽，实际却加了 30 槽，所有 oracle 时间点晚了 24 槽，即 120 秒。核心读取 `collector.SlotOf(now_us) + 1`，不会再替调用方换算这个偏移。

这不是本次 seed/meta 修复引入的回退，而是此前新增可调 SLOT_S 后遗留、在本轮复核发现的单位错误。meta 能证明配置一致，却不能修正 oracle 文件中的时间坐标。已有 5 秒 phase-1 sweep 没有运行 oracle 策略，其非 oracle 结果不受此问题影响。

**实际验证：**构造测量时刻 0 ms、5,000 ms 的两个 read，调用真实 make_oracle。SLOT_S=1、offset=30 输出槽 30/35；SLOT_S=5、offset=30 输出槽 30/31；将后者偏移改为 6 后输出正确的槽 6/7。

**最小修复：**按槽宽换算暖机偏移；更稳妥的是在 make_oracle 中接收暖机时长，将其加到原始时间戳后再分槽，使不能整除的 slot 也能对齐。不要仅修改 meta 而继续生成错位的数据。

**验收：**同一测量 trace 在 warmup=30 秒时，SLOT_S=1 的首个测量槽为 30，SLOT_S=5 时为 6；应同时验证后续槽。若允许任意槽宽，再覆盖不能整除暖机时长的例子。只需极小 trace，不需要重跑性能矩阵。

## 验证记录与边界

- 当前项目重新配置及构建 `leaper_bench_rocksdb`、`core_check`、`mapper_check`、`budget_check`、`sst_warm_check` 通过。
- `core_check`、`mapper_check`、`budget_check`、`sst_warm_check` 均通过。预算测试的无预算对照为 355，限额 32 的两个用例均为 32。
- `scripts/check_pristine_rocksdb_build.sh` 通过：未打补丁头文件下，适配器与 benchmark 均完成语法编译。这不等同于在干净机器重新链接并运行完整 pristine RocksDB。
- oracle 检查覆盖实际工具生成、小 trace 复用、seed/slot 改变拒绝、缺 meta 拒绝、新 TAG 分离、非 oracle 模型复用；benchmark 使用 stub，不将其当作性能验证。
- 已打补丁 RocksDB prepop 真实短跑：30,000 keys，1 秒 warmup + 3 秒测量，约 10k ops/s，退出 0；warmed_blocks=12,364，prepop_rejected=15,584，warm_open_failed=0。仅作功能检查。
- 新增 chains 与相关脚本通过 shell 语法检查；没有执行完整 M10 专机工作单，也没有验证 Linux direct-read 的延迟或性能结论。工作单中的部署、格式化、提交及推送指令仅作为审阅材料，不执行。

复现材料在本机 `/Users/yang/Documents/Codex/2026-09-19/ji/work/leaper-m9-recheck/` 与 `work/leaper-oracle-recheck/`。修复 agent 可按本报告的最小场景重建；无需依赖这些临时目录。

上述三个待修复项均经独立 reviewer 核对源码与复现证据。本轮仅新增此报告，未修改实现、实验结果或已有引擎补丁。
