# Leaper M9 修复最终验收（2026-09-22）

验收提交：`6be8a08`。对照上一轮 `docs/code-review-m9-followup-2026-09-22.md`，聚焦剩余 F1、F2、F3 及相关回归，继续按研究原型尺度检视。

**结论：通过。上一轮剩余的 3 项均已修复，可以关闭；此前 M9 检视提出的问题现已全部关闭。本轮范围内未发现需要继续修改的阻塞问题。**

## 逐项核对

| 项目 | 实现与实际验证 | 结论 |
|---|---|---|
| F1：CMake 保留旧补丁检测结果 | 检测提取到 `cmake/DetectPrepopFilter.cmake`，每次先清除普通变量及 cache 再探测。实际调用该模块，在同一构建目录依次切换 pristine → patched → pristine 头文件，结果为 absent → present → absent | 关闭 |
| F2：显式空模型后缀被错误替换 | 改为 `${MODEL_TAG_SUFFIX-$SUF}`。真实 chain 配合末端 stub 验证：显式空时 `_r1` 输出复用无后缀模型；未设置时跟随输出后缀；非空时使用指定后缀；repeat 的两轮输出分别为 `_r1`、`_r2`，均复用无后缀模型 | 关闭 |
| F3：oracle 暖机偏移单位错误 | run_m4 改传 `--warmup_s=30`；make_oracle 在原始时间戳上加暖机时长后再分槽。真实工具处理 0 ms、5,000 ms 两条 read：1 秒槽输出 30/35，5 秒槽输出 6/7，7 秒槽输出 4/5，覆盖不能整除暖机时长的情况 | 关闭 |

独立 reviewer 复核了实现、调用链及新增检查的覆盖范围，未发现上述修复引入明显回归。F3 保留旧的 `--slot_offset` 参数兼容旧调用；新的 run_m4 调用已使用秒数接口。M9 文档记录此前错误 oracle 已删除，已有实验表格没有使用非 1 秒 oracle，因此不要求重跑已有结果。

## 实际执行的检查

五个脚本均通过：

```sh
bash scripts/check_prepop_detection.sh
bash scripts/check_pristine_rocksdb_build.sh
bash scripts/check_chain_suffixes.sh
bash scripts/check_oracle_offset.sh
bash scripts/check_run_m4_oracle_binding.sh
```

当前项目重新配置明确重新执行了 `LEAPER_HAVE_PREPOP_FILTER` 探测，结果为 Success / present。构建以下目标成功：`leaper_bench_rocksdb`、`core_check`、`mapper_check`、`budget_check`、`sst_warm_check`。

实际执行 `core_check`、`mapper_check`、`budget_check`、`sst_warm_check` 均通过。预算测试无预算对照为 355，限额 32 的空范围与有键范围用例均为 32；SST 暖区读取命中共享缓存且文件读取为 0，控制区发生文件读取。相关 shell 语法检查及 `git diff --check` 通过。

## 验收边界

本轮验证的是修复是否满足上一轮要求。pristine 检查包含能力探测与适配器/benchmark 语法编译，没有重建完整的 pristine RocksDB；实验链使用 stub 验证参数传递，没有运行完整 M10 专机实验。未重新训练模型或测量性能，上一轮已通过的 prepop 功能短跑结论沿用，本次没有修改该运行时实现。

本轮仅新增此报告，未修改实现、实验数据或引擎补丁。无需为关闭本轮检视项继续返工。
