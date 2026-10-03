# 实验日志

格式见 CLAUDE.MD。每个优化（包括失败或无效的）各记一条。数字只来自实际运行的输出，
原始输出在 `results/`，文件名带日期、commit 和环境标签。VM 下只做相对比较。

## [2026-10-03] v0: baseline（参考实现）

- 假设：不适用（这是起点）。v0 追求的是"一眼能看出正确"：每侧一个 `std::map<Price, Level>`，Level 内是 `std::list<Order>` 加 `total_qty`，再用 `std::unordered_map<OrderId, Loc>` 做索引。这里记录它的指令数、访存和分配特征，作为后续所有版本的对照。
- 改动：见 commit `136910c`（v0）及其后的测量框架。
- 环境：vm（VMware，i5-1035G1，4 vCPU，**硬件 PMU 不可用**：`perf stat -e cycles` 返回 `<not supported>`；TSC 实测 1.1904 GHz），绑核 CPU 3，见 `results/env_20261003-2305_8471f71_vm.txt`。
- commit：cachegrind/heaptrack 的合成数据部分在 `8471f71` 上跑；其余在 `8b799fa` 上跑。`8b799fa` 只改了 `scripts/fetch_itch.sh`，被测的二进制代码相同。

### 结果

Workload：`syn_*` 由 `scripts/gen_workloads.sh` 生成（sha256 见 `results/workloads_sha256.txt`）；`itch12302019_*` 来自 NASDAQ ITCH 5.0 的 2019-12-30 数据，由 `scripts/itch_convert.sh` 转换。

| workload | ops | Ir | Ir/op | D1 miss rate | LL misses | 分配次数 (heaptrack) | gbench median ns/op (CV) |
|---|---:|---:|---:|---:|---:|---:|---:|
| syn_default | 1,000,000 | 464,224,729 | 464.2 | 2.1% | 379,437 | 993,497 | 199.5 (5.21%) |
| syn_aggressive | 1,000,000 | 473,034,928 | 473.0 | 1.8% | 379,430 | 994,437 | 204.3 (2.54%) |
| syn_deep | 1,000,000 | 473,932,781 | 473.9 | 2.5% | 410,467 | 1,008,096 | 283.1 (4.34%) |
| itch AAPL | 1,609,147 | 908,458,419 | 564.6 | 1.4% | 703,814 | 1,775,571 | 410.8 (5.47%) |
| itch SPY | 2,249,458 | 1,137,663,016 | 505.7 | 0.9% | 851,478 | 2,327,423 | （未跑 bench） |
| itch QQQ | 2,483,220 | 1,306,456,724 | 526.1 | 0.9% | 964,057 | 2,590,750 | 287.7 (2.23%) |

- cachegrind 的缓存参数固定为 I1 32K/8w、D1 48K/12w、LL 6M/12w，只统计回放区间。
- bench：20 次重复，随机交错（`results/bench_20261003-2334_8b799fa_vm.json`）。

延迟 harness（单位 cycles，包含约 58 cycles 的计时开销；5 轮，丢弃 warmup；VM 中仅作参考）：

| workload | class | p50 | p99 | p99.9 | p99.99 | max |
|---|---|---:|---:|---:|---:|---:|
| syn_default | all | 283 | 726 | 1,335 | 60,011 | 929,036 |
| syn_default | push_rest / push_trade / cancel / modify | 259 / 339 / 308 / 298 | 583 / 1,195 / 656 / 1,068 | | | |
| itch AAPL | all | 488 | 1,354 | 3,414 | 82,658 | 3,816,080 |

### 解释（cachegrind / perf / heaptrack）

1. **内存分配是指令数的最大头。** 在 syn_default 上，`malloc.c` 占 30.5% 的 Ir（141.5M / 464.2M）。heaptrack 统计到每个 op 约 1 次分配调用（syn 0.99 次，AAPL 1.10 次），对应 `list` 节点、`unordered_map` 节点，以及新价位的 `map` 节点。
2. **D1 未命中集中在索引的哈希查找上。** syn_default 上，`__hash_table` 一个文件就占 D1 读未命中的 60.6%（1,831,270 / 3,019,737）。按函数统计（含内联代码）：syn_default 上 `cancel` 占 48.1%，ITCH AAPL 上 `push` 占 47.8%（`index_.contains(id)` 的重复 id 检查）。perf（cpu-clock:u）的结果一致：syn 上排第一的是 `cancel`（18.2%），AAPL 上排第一的是 `push`（27.8%）。
3. **LL 未命中有一个由输入数据决定的下限。** `replay.hpp`（顺序读 op 数组）的 LL 读未命中是 375,001 次，等于 24,000,000 B / 64 B = 375,000 行。这是强制未命中，cachegrind 不模拟硬件预取。比较不同版本时，这部分是固定成本，应当扣除，或者只看差值。
4. **ITCH 回放完全不走撮合路径。** 用 v0 校验：AAPL、SPY、QQQ 都是 trades=0、rejected=0，并且收盘时簿被清空（final_orders=0）。所以 ITCH workload 考察的是挂单、撤单和减量路径；撮合路径要靠 `syn_aggressive`（trades/op = 0.30）来覆盖。

### 意外与遗留问题

- **合成生成器的第一版模型是错的。** 固定撤单概率时，参数扫描显示 p_cancel=0.42 时 100 万步后簿上只剩 37 单，0.40 时涨到 35,809 单，没有稳态。原因是守恒：稳态下撤单率 ≈ 挂单率 − 成交率。所以改成按订单寿命驱动撤单（M/M/∞），撤单比例由模型决定：default 下 cancel/push=0.87。
- **cachegrind 的 Ir 并不是逐条确定的。** 同一个 workload 两次运行，Ir 分别是 464,224,722 和 464,224,729，差 7 条。比较版本时，小于约 0.01% 的差异按噪声处理。
- **墙钟时间和 Ir 不成比例。** AAPL 的 ns/op 是 QQQ 的 1.43 倍（410.8 和 287.7），但 Ir/op 只高 7%（564.6 和 526.1）；同时 AAPL 的 D1 miss rate 更高（1.4% 和 0.9%）。差距主要来自访存，而 cachegrind 的简单缓存模型没有反映真实的延迟。没有 PMU 时，这个差距在 VM 里无法直接验证。（2026-10-04 开启 PMU 后已有硬件数据支持，见下一节。）
- **假设（未验证，留给 v1 之后）**：libc++ 的 `std::hash<uint64_t>` 是恒等函数。合成数据的 id 是连续的，桶相邻、局部性好；ITCH 的 order ref 在单个标的内是稀疏的，所以 AAPL 上 `push` 的未命中更多。可以通过换一个 hash 或换成开放寻址表来对照验证。
- **延迟尾部被 VM 噪声主导。** p99.99 在 6–8 万 cycles，max 达到百万 cycles 量级。VM 里只看 p50 和 p99。
- **最初的 ITCH 下载没有走代理。** wget 和 aria2c 不认大写的 `HTTPS_PROXY`。已在 `8b799fa` 修复。

### 补充：硬件计数器（2026-10-04，环境：vm，VMware 开启虚拟化性能计数器后）

- commit：`79fb520`。结果文件名带 `-dirty`，因为工作区里有一处未提交的改动：`include/lob/types.hpp` 的格式调整和新增的 static_assert，不影响生成的代码。
- 工具：`book counters`（进程内 perf_event_open，只统计回放区间，5 轮取中位数）、`scripts/perf.sh stat|topdown`（perf `--control` ROI，每组 ≤4 个可编程事件，5 次取均值）。PMU 能力见 `results/env_*_79fb520*`：没有 PEBS，没有 `-M TopdownL1`，没有通用 LLC 事件。

| workload | cycles/op (spread) | instr/op | IPC | branch-miss/op | L1d miss/op | dTLB miss/op |
|---|---:|---:|---:|---:|---:|---:|
| syn_default | 232.6 (4.0%) | 458.9 | 1.97 | 1.97 | 5.15 | 0.017 |
| syn_aggressive | 247.3 (3.5%) | 467.5 | 1.89 | 2.27 | 4.44 | 0.018 |
| syn_deep | 363.7 (10.6%) | 469.7 | 1.29 | 2.18 | 6.21 | 0.119 |
| itch AAPL | 516.4 (21.4%) | 558.4 | 1.08 | 3.27 | 5.58 | 0.305 |
| itch QQQ | 383.7 (19.5%) | 520.6 | 1.36 | 2.31 | 3.21 | 0.095 |

`perf.sh stat`（回放区间，5 次均值）：

| 事件 | syn_default | itch AAPL |
|---|---:|---:|
| instructions | 458,688,857 | 898,885,949 |
| L1-dcache-loads | 117,719,442 | 229,014,435 |
| mem_load_retired.l1_miss / l2_miss / l3_miss | 2,540,996 / 156,006 / 36,911 | 4,213,257 / 2,007,138 / 845,242 |
| cycle_activity.stalls_mem_any / cycles | 29.8M / 223.7M（13%） | 230.8M / 771.9M（30%） |
| cycle_activity.stalls_l3_miss / cycles | 3.9M（1.7%） | 76.8M（10%） |
| TopDown L1 近似值（FE / BadSpec / Retiring / BE） | 16.5 / 29.7 / 41.8 / 12.0 % | 14.3 / 30.0 / 23.9 / 31.8 % |

解释：
- **ROI 和 cachegrind 互相印证。** `L1-dcache-loads` 和 cachegrind 统计的读次数基本相等：syn 是 117,719,442 对 117,719,326，AAPL 是 229,014,435 对 229,013,294。instructions/op（458.9）和 cachegrind 的 Ir/op（464.2）接近。两者不完全相等，是因为 release 用 `-march=native` 编译，profile 用 x86-64-v3，生成的代码不同。
- **AAPL 慢在访存，不在指令数。** 和 syn_default 相比，AAPL 的 L3 未命中每 op 多约 14 倍（0.525 对 0.037），访存 stall 占周期的比例从 13% 升到 30%，TopDown 的 Backend Bound 从 12% 升到 32%，IPC 从 1.97 降到 1.08。这解释了上一节"墙钟时间和 Ir 不成比例"的问题：cachegrind 能数出 D1 未命中，但看不到每次未命中要等多少周期。
- **新的假设（未验证）：AAPL 盘中在簿订单数和价位数远多于合成数据（约 1.8k 单），工作集超出了 L2/L3。** 验证方法：在 `book validate` 里统计峰值订单数和价位数。

意外与遗留问题：
- **VM 里 cycles 的 spread 很大**（AAPL 21%，QQQ 20%），instructions 的 spread 是 0–0.5%。比较版本时，cycles 只能看多轮中位数的相对变化。
- **AAPL 的 instructions 在各轮之间并不完全相同**（spread 0.5%），合成数据是 0.0%。原因未查。
- **TopDown 的 Bad Speculation 约 30%，看起来偏高。** branch-miss 只有 2–3 次/op，而公式依赖 `uops_issued.any` 和 `uops_retired.slots` 的计数口径一致，在 VM 里没法和 perf-metrics 对照。这个值只用于同一台机器上不同版本之间的相对比较。

