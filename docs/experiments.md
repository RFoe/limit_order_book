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
- 工具：`book counters`（进程内 perf_event_open，只统计回放区间，5 轮取中位数）、`scripts/perf.sh stat|topdown`（perf `--control` ROI，每组 ≤4 个可编程事件，5 次取均值）。PMU 能力见 `results/env_20261004-0109_1a1582b-dirty_vm.txt`：没有 PEBS，没有 `-M TopdownL1`，没有通用 LLC 事件。

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

## [2026-10-04] v1: 订单索引换成 boost::unordered_flat_map

- **假设**：根据 v0 开启 PMU 后的数据，AAPL 的瓶颈在等内存：IPC 1.08，30% 的周期 stall 在内存上。按 `mem_load_retired.l3_miss` 采样，51% 的 L3 未命中、55% 的 L2 未命中落在 `push`。annotate 显示 83% 的样本集中在 `index_.contains(id)` 遍历哈希桶链表时读取 `__hash_` 的那条 load。原因是 `std::unordered_map` 给每个订单单独分配一个堆节点，每次查找都要追一次指针；AAPL 的峰值是 27,110 个在簿订单，工作集超出了 L2。换成开放寻址表后，槽位连续存放、不需要逐节点分配，预期：
  - 分配次数：AAPL 每个挂单少一次，1.78M → 约 0.98M
  - `__hash_table` 相关的 Ir 和 D1 读未命中大幅下降
  - AAPL 的 L2/L3 未命中、stall 下降，IPC 上升
  - syn_default 的提升主要来自指令数减少
- **改动**：commit `9688ed7`。`include/lob/v1/book.hpp` 是从 v0 复制来的，唯一的实质改动是 `using Index = boost::unordered_flat_map<OrderId, Loc>;`（boost 1.90，vcpkg 包 `boost-unordered`）。CMake 增加了 `find_package(boost_unordered)` 并链接 `Boost::unordered`。
- **正确性**：debug 下 ctest 24/24 通过（ASan+UBSan，每一步检查不变量）。release 下差分测试 30 个 seed × 20 万 op，三种生成画像全部一致。ITCH AAPL/SPY/QQQ 和 syn_aggressive 的回放 checksum 与 v0 相同。
- **环境**：vm，绑核 CPU 3，`results/env_20261004-0321_9688ed7-dirty_vm.txt`。结果文件名带 `-dirty`，是因为工作区里有一处尚未提交的 `include/lob/types.hpp` 格式调整，不影响生成的代码。

### 结果（v0 → v1）

cachegrind（确定性，只统计回放区间；profile 构建）：

| workload | Ir | D1 读未命中 | D1 写未命中 | LLd 未命中（读+写） |
|---|---:|---:|---:|---:|
| syn_default | 464.2M → 290.8M (**−37.4%**) | 3.02M → 1.55M (−48.6%) | 0.98M → 1.12M (+13.5%) | +0.6% |
| syn_aggressive | 473.0M → 304.7M (−35.6%) | −49.0% | +13.6% | +0.0% |
| syn_deep | 473.9M → 297.1M (−37.3%) | −22.1% | +40.0% | +2.0% |
| itch AAPL | 908.5M → 623.3M (**−31.4%**) | 4.53M → 2.64M (−41.6%) | 0.45M → 1.42M (**+217.6%**) | +20.8% |
| itch SPY | 1137.7M → 737.9M (−35.1%) | −62.5% | +203.5% | +0.9% |
| itch QQQ | 1306.5M → 864.5M (−33.8%) | −47.9% | +522.6% | +2.6% |

heaptrack（分配调用次数）：syn_default 993,497 → 497,393（−49.9%）；AAPL 1,775,571 → 979,877（**−44.8%**，预测约 0.98M）；QQQ 2,590,750 → 1,369,743（−47.1%）。峰值堆内存：AAPL 41.74M → 42.98M。

硬件计数器（`book counters`，同一进程内 v0/v1 交替运行，7 轮取中位数，单位为每 op）：

| workload | cycles | instructions | IPC | branch-miss | L1d miss* | dTLB miss |
|---|---:|---:|---:|---:|---:|---:|
| syn_default | 222.5 → 150.3 (−32.5%) | −37.9% | 2.06 → 1.90 | −10.5% | −16.7% | −10.9% |
| syn_aggressive | 228.3 → 155.7 (−31.8%) | −35.9% | 2.05 → 1.93 | −10.0% | −24.6% | −26.9% |
| syn_deep | 324.1 → 178.1 (−45.0%) | −37.7% | 1.45 → 1.64 | −19.2% | −5.8% | −62.0% |
| itch AAPL | 516.8 → 301.7 (**−41.6%**) | −31.9% | **1.08 → 1.26** | −15.8% | −18.6% | −75.8% |
| itch QQQ | 321.0 → 200.8 (−37.4%) | −34.4% | 1.62 → 1.70 | −28.3% | −9.0% | −67.0% |

\* 内核把 generic 的 L1d read miss 映射成 Icelake 的 L1D.REPLACEMENT，统计的是"L1 换入了多少行"，写操作导致的换入也算在内。所以它要和 cachegrind 的"读+写未命中"对比：AAPL 上 cachegrind 是 4.98M → 4.07M（−18%），和这里的 −18.6% 一致。

`perf.sh stat`（只统计回放区间，5 次取均值）：

| 事件 | AAPL v0 → v1 | syn_default v0 → v1 |
|---|---:|---:|
| mem_load_retired.l1_miss | 4.25M → 1.99M (−53.3%) | −54.5% |
| mem_load_retired.l2_miss | 2.01M → 0.30M (**−85.1%**) | −69.5% |
| mem_load_retired.l3_miss | 784k → 86k (**−89.0%**) | −48.7% |
| cycle_activity.stalls_mem_any / cycles | 28.5% → 21.3% | 12.2% → 12.0% |
| cycle_activity.stalls_l3_miss | 66.5M → 10.9M (−83.7%) | −40.9% |
| dtlb_load_misses.walk_completed | −81.4% | −30.8% |
| TopDown 近似值（FE / BadSpec / Ret / BE） | 13.7/30.1/23.9/**32.2** → 17.0/34.6/25.8/**22.6** % | 16.2/29.6/42.7/11.4 → 14.0/37.3/40.4/8.3 % |

Google Benchmark（20 次重复，随机交错，中位数）：

| workload | v0 ns/op (CV) | v1 ns/op (CV) | 变化 |
|---|---:|---:|---:|
| syn_default | 193.9 (4.08%) | 127.8 (1.88%) | −34.1%（1.52x） |
| syn_aggressive | 200.9 (4.50%) | 138.1 (2.15%) | −31.3%（1.45x） |
| syn_deep | 272.1 (4.16%) | 152.9 (2.06%) | −43.8%（1.78x） |
| itch AAPL | 396.4 (5.24%) | 258.0 (2.30%) | −34.9%（1.54x） |
| itch QQQ | 276.4 (2.09%) | 175.1 (1.81%) | −36.7%（1.58x） |

延迟（cycles，包含约 58 cycles 的计时开销，5 轮，VM 中仅作参考）：

| | p50 | p99 | p99.9 |
|---|---:|---:|---:|
| syn_default all | 287 → 228 | 744 → 479 | 1,339 → 837 |
| AAPL all | 508 → 372 | 1,554 → 926 | 3,696 → 2,668 |
| AAPL push_rest | 533 → 341 | 1,635 → 882 | 4,416 → 2,480 |

### 解释

- **节省来自哪里**（`cg_annotate --diff`，AAPL）：
  - `__hash_table`：Ir −221.0M，D1 读未命中 −3.03M
  - `malloc.c`：Ir −110.8M，因为哈希节点不再分配
  - 新增 boost `foa/core.hpp`：Ir +101.7M，D1 读未命中 +0.81M
  - 新增 `mulx.hpp`：Ir +6.9M。这是 boost 对 `boost::hash<uint64_t>`（恒等函数）结果做的再混合，正好解决了 v0 遗留假设里说的 id 分布问题
  
  净效果是指令数 −31%，D1 读未命中 −42%。原始文件见 `results/cachegrind_*9688ed7*_diff_v0_v1.txt`。
- **为什么 AAPL 收益最大**：v0 在 AAPL 上的瓶颈是索引节点的 L2/L3 未命中。v1 把 L3 未命中降低了 89%，stall_l3 降低了 84%，所以 cycles 的降幅（−41.6%）大于 instructions 的降幅（−31.9%），IPC 从 1.08 升到 1.26。syn_default 本来就不太受内存限制，stall 占比也没变（12.2% → 12.0%），收益基本来自指令变少，所以 IPC 反而从 2.06 降到 1.90：剩下的代码里，追指针的比例更高了。
- **预测与实测**：分配次数预测约 0.98M，实测 979,877，相符。L2/L3 未命中、IPC 的变化方向都和预测一致。

### 意外与遗留问题

- **D1 写未命中大幅上升**（AAPL +1.11M，几乎全部来自 `pair.h`，即把 `pair<OrderId, Loc>` 写进槽位）。v0 往刚 malloc 出来的节点写入，而 glibc tcache 是后进先出的，复用的是刚释放、还在缓存里的内存；v1 是按哈希值往一个大数组的随机槽位写入，所以更容易未命中。代价被读未命中的下降抵消掉了，但它说明 v1 的写路径对缓存并不友好。cachegrind 里 AAPL 的 LL 写未命中也从 72k 升到 186k。
- **扁平表按峰值容量分配，之后不会缩小**，峰值堆内存略有增加。
- **TopDown 的 Bad Speculation 占比上升（30% → 35%）**，但 branch-miss 的绝对次数下降了 16%。占比上升只是因为总周期（分母）变少了。这个近似公式本身仍然只适合做相对比较。
- **同一份 AAPL workload，两种方式测出的 instructions 不完全一致**：进程内多轮（`book counters`）测到 v1 为 611.8M，新进程（perf stat）测到 620.0M，相差约 1.3%。推测与 malloc 的堆状态有关，未查证。
- **`perf record` 推算出的事件总数不能当作绝对值**：按 l3_miss 采样时，推算的总数远低于 perf stat 的精确计数（dmesg 里有采样降频的提示），所以只看比例分布。为此给 `perf.sh` 加了 `LOB_PERF_PERIOD`，稀有事件可以按固定间隔采样。
- **v2 的线索**：v1 在 AAPL 上剩下的 L3 未命中样本里，`rest`（插入 map 和 list）占 29%，删除价位/链表节点的 `erase` lambda 占 22.5%，内联到回放循环里的扁平表查找占 25%，malloc 约 10%。cycles 采样里 `rest` 占 29%，`erase` 占 18%。瓶颈已经转移到价位树（`std::map`）和订单链表节点（`std::list`）上，下一步的候选是订单节点池（或侵入式链表）以及价位结构。

## [2026-10-04] v2: 订单链表换成侵入式下标链表 + 节点池

- **假设**：v1 之后，AAPL 剩下的 L3 未命中采样里，`rest` 和 `erase` 占 51%，malloc 约 10%；heaptrack 显示每个挂单仍要为 `std::list` 节点分配一次。把订单节点放进一个连续的池里，节点之间用 `uint32_t` 下标相连，空闲节点按后进先出复用，预期：
  - 分配次数：AAPL 979,877 → 约 18 万（只剩 map 节点），syn 降到几千
  - malloc 和 list 相关的 Ir 消失
  - `rest`/`erase` 里的 L3 未命中只部分下降，因为 `std::map` 的节点还在
- **改动**：commit `9f38b37`，只替换 `std::list<Order>`。
  - `OrderNode{id, qty, next, prev}` 为 24 字节，`static_assert(sizeof <= hardware_destructive_interference_size / 2)`，该值实测为 64。
  - `Level` 改为 `head/tail/total_qty/count`。
  - 所有节点放在一个 `std::vector` 池里，池上穿着两种链表：每个价位的 FIFO 双向链表，和后进先出的空闲单链表。
  - 不变量检查新增：prev/tail/count 一致，空闲节点数 + 在簿节点数 = 池的大小。
  - `std::map` 价位树和 `flat_map` 索引都没有改动。`Loc` 的字段顺序也故意保持不变（重排后可以从 24 字节压到 16 字节，是另一项优化）。
- **正确性**：debug 和 release 的 ctest 都是 25/25。差分测试 30 个 seed × 20 万 op 全部一致。6 个 workload 上 v0/v1/v2 的回放 checksum 相同。
- **环境**：vm，绑核 CPU 3，commit `9f38b37`（工作区干净），`results/env_*9f38b37*`。

### 结果（v1 → v2；累计相对 v0）

cachegrind（profile 构建，只统计回放区间）：

| workload | Ir v1 → v2 | 累计 Ir（相对 v0） | D1 读未命中 | D1 写未命中 |
|---|---:|---:|---:|---:|
| syn_default | 290.6M → 224.4M (**−22.8%**) | −51.7% | −7.3% | +5.7% |
| syn_aggressive | 304.6M → 240.0M (−21.2%) | −49.3% | −7.9% | +11.5% |
| syn_deep | 296.9M → 230.4M (−22.4%) | −51.4% | −9.3% | +1.8% |
| itch AAPL | 622.9M → 516.8M (**−17.0%**) | −43.1% | −8.6% | −1.4% |
| itch SPY | 737.4M → 595.3M (−19.3%) | −47.7% | −14.4% | −20.3% |
| itch QQQ | 863.9M → 705.4M (−18.3%) | −46.0% | −12.1% | −5.9% |

heaptrack（分配次数）：syn_default 497,393 → **1,303**（−99.7%）；AAPL 979,877 → **184,181**（−81.2%，预测约 18 万）；QQQ 1,369,743 → 148,662（−89.1%）。峰值堆内存基本不变（AAPL 42.98M → 42.87M）。

硬件计数器（`book counters`，v0/v1/v2 在同一进程内交替，7 轮取中位数，单位为每 op）：

| workload | cycles v0 / v1 / v2 | v1→v2 | instructions v1→v2 | IPC v0 / v1 / v2 | branch-miss v1→v2 |
|---|---:|---:|---:|---:|---:|
| syn_default | 223.0 / 146.3 / 123.0 | −16.0% | −23.4% | 2.06 / 1.95 / 1.78 | −4.8% |
| syn_aggressive | 228.5 / 158.1 / 135.5 | −14.3% | −21.7% | 2.05 / 1.90 / 1.74 | −4.9% |
| syn_deep | 327.3 / 175.3 / 140.5 | −19.9% | −23.3% | 1.43 / 1.67 / 1.60 | −5.0% |
| itch AAPL | 483.8 / 298.4 / 277.9 | **−6.9%** | −17.5% | 1.15 / 1.28 / **1.13** | **+6.5%** |
| itch QQQ | 321.6 / 198.4 / 183.1 | −7.7% | −18.8% | 1.62 / 1.72 / 1.52 | +11.1% |

`perf.sh stat`（回放区间，5 次取均值，v1 → v2）：

| 事件 | itch AAPL | syn_default |
|---|---:|---:|
| cycles | −4.1% | −23.6% |
| instructions | −17.2% | −22.9% |
| mem_load_retired.l2_miss | −10.7% | −70.0% |
| mem_load_retired.l3_miss | −29.7% | −49.8% |
| cycle_activity.stalls_mem_any | **+7.2%** | −19.9% |
| cycle_activity.stalls_l3_miss | −1.3% | −31.0% |
| stall_mem 占 cycles 的比例 | 23.1% → **26.8%** | 13.7% → 13.9% |
| TopDown BE（近似值） | 24.8% → 26.3% | 8.0% → 10.1% |

Google Benchmark（20 次重复，v0/v1/v2 随机交错，中位数）：

| workload | v0 ns/op (CV) | v1 | v2 | v1→v2 | 累计 v0→v2 |
|---|---:|---:|---:|---:|---:|
| syn_default | 199.1 (3.4%) | 128.5 (4.2%) | 106.7 (3.6%) | **−17.0%** | 1.87x |
| syn_aggressive | 204.4 (4.8%) | 138.7 (4.1%) | 115.5 (4.9%) | −16.7% | 1.77x |
| syn_deep | 275.4 (5.1%) | 151.3 (3.7%) | 122.4 (4.2%) | −19.1% | **2.25x** |
| itch AAPL | 408.9 (3.2%) | 257.2 (3.4%) | 246.1 (5.0%) | −4.3%（未超出 CV） | 1.66x |
| itch QQQ | 284.9 (6.4%) | 174.7 (3.6%) | 163.4 (3.0%) | −6.5% | 1.74x |

延迟（cycles，包含计时开销，5 轮，VM 中仅作参考）：

| | p50 v0/v1/v2 | p99 v0/v1/v2 | p99.9 v0/v1/v2 |
|---|---:|---:|---:|
| syn_default all | 289 / 230 / 215 | 721 / 498 / 411 | 1,372 / 912 / 841 |
| AAPL all | 496 / 376 / 347 | 1,461 / 917 / **792** | 3,529 / 2,573 / 2,162 |

### 解释

- **节省来自哪里**（`cg_annotate --diff` v1→v2，AAPL）：malloc.c 的 Ir 减少 111.0M，未符号化的 libc++ `operator new/delete` 包装层（`???`）减少 27.8M，`<list>` 减少 14.7M，`allocate.h` 和 `arena.c` 合计减少 6.9M；新增的侵入式链表和池的代码（`book.hpp`）增加 48.3M，`vector.h` 增加 8.2M。syn 的情况相同：malloc 减少 70.7M，book.hpp 增加 30.8M。原始文件见 `results/cachegrind_*9f38b37*_diff_v1_v2.txt`。
- **为什么 syn 收益大、AAPL 收益小**：
  - 去掉的主要是 malloc/free 的指令，这些指令走 tcache、都在缓存里，执行得很快（IPC 高）。所以指令数降了 17–23%，而 cycles 的降幅取决于 workload 有多受内存限制。
  - syn_default 的 stall 占比只有 14%，cycles 和指令数几乎同步下降（−23.6% 对 −22.9%）。
  - AAPL 在 v2 之后有 27% 的周期 stall 在内存上。L3 未命中虽然少了 30%，但因 L3 未命中而 stall 的周期几乎没变（−1.3%），所以 cycles 只降了 4–7%，IPC 降到 1.13。
- **AAPL 剩下的 stall 在哪**：v2 的 L3 未命中采样中，`rest`（37%）和 `erase`（22%）占了大部分。annotate 显示，这两个函数内部的样本集中在 `__tree:2049/2050/1594`，也就是 `std::map` 向下查找时读取节点 key 并比较的指令。池节点相关的 `alloc_node` 只占 1%。AAPL 有 5,320 个价位、树深约 12 层，而且价位节点是 malloc 分配的，散落在堆里。
- **预测与实测**：AAPL 分配次数实测 184,181，预测约 18 万，相符。malloc/list 相关的 Ir 消失，相符。AAPL 的 cycles 收益受价位树的访存 stall 限制，也和预期一致（预期就是"只部分下降"）。

### 意外与遗留问题

- **AAPL 的墙钟提升没有超出噪声**：bench 是 −4.3%，而 CV 是 3.4% 和 5.0%。进程内交替测量的 cycles 中位数是 −6.9%（spread 3.6% / 1.3%），perf stat 是 −4.1%，可以判断有小幅的真实改善，但 VM 里无法精确定量。
- **IPC 全面下降**（AAPL 1.28 → 1.13）：去掉的是高 IPC 的 malloc 代码，剩下的部分访存密度更高。IPC 下降本身不是退化，要看 cycles。
- **branch-miss 在 ITCH 上上升**（AAPL +6.5%，QQQ +11.1%），在 syn 上下降。可能和侵入式链表里 `prev/next == kNil` 的分支模式有关，未查证。
- **D1 写未命中在 syn 上上升 6–12%**。按函数看，链表删除时的写未命中只是转移了位置（v2 的 unlink/free +280k，`std::list` 的删除 −340k）；但索引槽位写入（`pair.h`，代码没变）在 syn 上增加了约 158k，在 AAPL 上基本持平。推测是缓存竞争带来的间接影响，未查证。
- **测量噪声**：同一份 v1 代码，cachegrind 的 Ir 在两轮之间差 0.08%（290.79M 对 290.57M），推测与 malloc 路径受堆状态影响有关。perf stat 里 v1 AAPL 的 cycles 和 L3 未命中，在两次会话之间也有 2–30% 的差别（469.7M 对 481.1M，86k 对 112k）。所以版本之间只比较同一会话、交替运行的数据。
- **延迟尾部的改善比中位数大**（AAPL p99 −14%，p50 −8%）。推测是去掉了 malloc 偶发的慢路径（`_int_malloc`、consolidation），未查证。
- **v3 的线索**：AAPL 剩余的访存 stall 集中在 `std::map` 价位树的查找上。候选方向：价位结构改为连续存储（比如有序数组，或"最优价附近一段连续数组 + 远端稀疏结构"，因为 AAPL 的价格跨度有约 20 亿 tick）。此外 `Loc` 可以重排到 16 字节，让索引槽位更小。

