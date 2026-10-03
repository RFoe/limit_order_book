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
- **墙钟时间和 Ir 不成比例。** AAPL 的 ns/op 是 QQQ 的 1.43 倍（410.8 和 287.7），但 Ir/op 只高 7%（564.6 和 526.1）；同时 AAPL 的 D1 miss rate 更高（1.4% 和 0.9%）。差距主要来自访存，而 cachegrind 的简单缓存模型没有反映真实的延迟。没有 PMU 时，这个差距在 VM 里无法直接验证。
- **假设（未验证，留给 v1 之后）**：libc++ 的 `std::hash<uint64_t>` 是恒等函数。合成数据的 id 是连续的，桶相邻、局部性好；ITCH 的 order ref 在单个标的内是稀疏的，所以 AAPL 上 `push` 的未命中更多。可以通过换一个 hash 或换成开放寻址表来对照验证。
- **延迟尾部被 VM 噪声主导。** p99.99 在 6–8 万 cycles，max 达到百万 cycles 量级。VM 里只看 p50 和 p99。
- **最初的 ITCH 下载没有走代理。** wget 和 aria2c 不认大写的 `HTTPS_PROXY`。已在 `8b799fa` 修复。
