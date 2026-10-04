# CLAUDE.md

## 项目目标

我准备去国内第一梯队的私募量化企业实习(HFT sysdev)。 </br>
这是一个**学习项目**：从零实现一个限价订单簿（limit order book）撮合引擎，然后通过 benchmark → profile → tune 的循环逐步优化，积累低延迟系统开发经验。
重点是**过程和理解**，而不是尽快得到一个很快的引擎。每一次优化都必须能用数据解释"为什么变快了"。

## 协作方式

- remote-git upstream: `git@github.com:RFoe/limit_order_book.git`
- **用中文交流**（对话、说明、实验日志都用中文；代码注释和 commit message 保持英文）。
- 我在学习：做设计决定或优化前，先用几句话说明思路、备选方案和取舍，再动手。
- **一次只做一个优化**。流程：说明假设和预期变化的指标 → 实现 → 差分测试通过 → 测量前后对比 → 用数据解释原因 → 写入实验日志 → 单独 commit。不要把多个优化混在一次改动里，否则无法归因。
- 不要编造或估计性能数字。所有数字必须来自本次实际运行的输出。
- 不确定的事（例如某个 perf 事件在 VM 里是否可用）先跑命令验证，再下结论。
- 某个组件如果可以直接复用开源实现，比如itch parser, benchmark, github CI/CD pipeline等，可以直接拿来用，我们的重点是系统调优的过程本身，以及经验积累

## 环境与限制

- VMware Workstation 中的 Ubuntu 24.04 LTS，宿主机是配置一般的笔记本(笔记本的问题主要是频率不稳, cpu governor, turbo, C-state, cgroup set,isolcpus、nohz_full、大页、绑核)。
- **硬件 PMU（2026-10-04 起已在 VMware 中开启虚拟化性能计数器）**：Icelake PMU，8 个通用 + 4 个固定计数器；cycles/instructions/branch/L1d/dTLB、`mem_load_retired.*`、`cycle_activity.*`、TopDown 原始事件可用。**不可用**：PEBS（`max_precise=0`，无 `:pp`、无 `perf mem`，采样有 skid）、`-M TopdownL1`（perf-metrics 未虚拟化）、通用 `LLC-*` 别名（用 `mem_load_retired.l3_miss` 代替）。每次开机后先 `scripts/env_tune.sh apply`（`perf_event_paranoid` 重启后回到 4），再用 `scripts/env_check.sh` 确认；探测结果以输出为准，不可用就退回 cachegrind，不要反复尝试。
- 墙钟计时噪声大，绝对延迟数字不可信，只做**相对对比**。
- 之后会把同一套代码和脚本放到租用的裸金属服务器上跑最终数字，因此：脚本不写死本机路径；不提交二进制；`-march=native` 的产物必须在目标机器上重新构建。

## 测量规范

我们需要先搭建自己的测量框架(order sequence generator, benchmark, ITCH parser, latency harness framework, profiling script, env script, .etc)，这是性能测试和优化的基础。
指标优先级：

1. **cachegrind 的指令数（Ir）和 D1/LL 未命中**：结果确定，不受 VM 噪声影响，是 VM 环境下的主指标。以及perf record/annotate + trace + heaptrack
   - **硬件计数器**（交叉验证 + cachegrind 看不到的东西：真实 stall、分支预测、预取效果）：`book counters`（进程内、版本交替、每 op 中位数）和 `scripts/perf.sh stat|topdown`（命名事件，每轮 ≤4 个可编程事件避免复用）。instructions 跨轮稳定（spread≈0%），cycles 受 VM 噪声影响（spread 数 %），只看相对变化。
2. **Google Benchmark 中位数**：`--benchmark_repetitions=20 --benchmark_report_aggregates_only=true`，看中位数和变异系数（CV），不看均值和单次结果。
3. **自研延迟 harness**：每次操作的延迟分布（p50 / p99 / p99.9 / max），用 `rdtscp` 计时，启动时校准 TSC 频率；先确认 `/proc/cpuinfo` 里有 `constant_tsc` 和 `nonstop_tsc`。VM 中仅作参考。

规则：

- A/B 对比时，多个版本在**同一进程、同一份数据**上交替运行，让它们承受相同的噪声。
- 运行 benchmark 时绑核：`taskset -c <cpu>`。
- benchmark 输出写到 `results/`，文件名包含日期、git short hash、环境标签（`vm` / `metal`）。
- 报告结果时注明环境。VM 下的结论表述为"相对 vX 指令数减少 N%"，不说"延迟是 N ns"。

## 构建与常用命令

工具链：clang 23 + libc++，C++26，CMake ≥ 3.25 + Ninja，vcpkg classic 模式（`$VCPKG_ROOT`，triplet `x64-linux-clang-libcxx`，需要 `catch2`、`benchmark`）。

| preset | 用途 | 关键 flag |
|---|---|---|
| `debug` | 正确性：ASan+UBSan，每个 op 后检查不变量（`LOB_CHECK_INVARIANTS`） | `-g -fsanitize=address,undefined` |
| `release` | benchmark / latency / perf / heaptrack | `-O3 -g -march=native` |
| `profile` | cachegrind（valgrind 3.22 不支持 AVX-512，native 会 SIGILL） | `-O3 -g -march=x86-64-v3` |

```bash
cmake --preset debug && cmake --build --preset debug && ctest --preset debug   # 全量测试(~1min)
cmake --preset release && cmake --build --preset release
LOB_DIFF_SEEDS=1,2,3 LOB_DIFF_OPS=200000 build/release/test_book "[diff]"     # 加大差分测试
LOB_DIFF_VERSIONS=v7 ctest --preset debug                                       # 只验证新版本（见"正确性"）

scripts/env_check.sh                 # 环境快照(只读) -> results/env_*.txt，含 PMU 是否可用
scripts/env_tune.sh apply|restore    # sudo: perf_event_paranoid / governor / turbo ...(重启后需重新 apply)
scripts/gen_workloads.sh             # data/syn_*.ops + results/workloads_sha256.txt
scripts/fetch_itch.sh [MMDDYYYY]     # ITCH 样本(默认 12302019, 3.5G, aria2c 多连接)
scripts/itch_convert.sh [gz] [SYM..] # zcat 流式转换 -> data/itch<day>_<SYM>.ops
scripts/cachegrind.sh [ops] [ver..]  # 主指标(VM)：Ir / D1 / LL，只统计回放区间
scripts/bench.sh                     # Google Benchmark，20 reps，随机交错，看 median + CV
scripts/latency.sh [ops] [vers]      # rdtscp 每 op 延迟分布(VM 中仅参考)
scripts/perf.sh record|stat|topdown|trace  # 只统计回放区间(--control ROI)；无 PMU 时 record 退回 cpu-clock:u
build/release/book counters --workload F  # 进程内硬件计数器，版本交替，每 op 中位数
scripts/heaptrack.sh [ops] [ver]     # 分配次数 / 峰值
```

`book` CLI：`gen` / `info` / `validate` / `itch2ops` / `replay` / `latency` / `versions`（见 `source/main.cxx` 头注释）。
所有脚本的输出写入 `results/<tool>_<YYYYmmdd-HHMM>_<hash>_<vm|metal>...`；`LOB_CPU` 指定绑核（默认最后一个核），`LOB_WORKLOAD` 指定 workload（逗号分隔）。

## 目录结构

```
include/lob/
  types.hpp events.hpp book_concept.hpp   公共词汇：Op(24B)、事件、Sink、OrderBook concept（所有版本共享）
  versions.hpp                            唯一的版本注册点：VersionList<v0::Book, ...>
  replay.hpp                              apply/replay（Debug 下逐步检查不变量）
  v0/book.hpp                             参考实现：map<Price, Level{list<Order>}> + unordered_map 索引
  vN/                                     后续优化版本：新建目录，永不覆盖旧版本
  workload/ itch/ harness/                二进制格式+生成器 / ITCH 5.0 解析+转换 / TSC 与延迟统计
source/   main.cxx(CLI) 与上面各模块的 .cxx
test/     语义测试、差分测试(diff_runner.hpp：比较+缩减)、workload、ITCH
benchmark/ bench_replay.cxx              每个 (版本 × workload) 一个 benchmark
scripts/  见上         docs/experiments.md 实验日志         results/ 测量输出(提交)    data/ workload(不提交)
```

**不要在 git 历史里覆盖旧版本**：新版本 = 新目录 `include/lob/vN/` + 在 `versions.hpp` 追加一项。这样所有版本能编进同一个二进制做对比，差分测试也始终能拿 v0 作参照。

## 订单簿接口约定

- 操作：`push`、`cancel`、`modify`。修改语义：只减少数量时保留时间优先；改价或增加数量等价于撤单后重新下单。市价单、IOC、FOK先不实现，只考虑limit order。
- 撮合规则：价格优先、时间优先；成交价取被动方（maker）的挂单价。

## 正确性

- v0 是作为正确性+优化加速比的参考实现。其他所有版本必须通过**差分测试**：同一订单流同时喂给 v0 和 vN，逐条比较事件序列，结束时再比较簿的状态（每一档的价格和总量）。
- 差分测试使用多个随机 seed，失败时打印 seed，并尽量给出最短复现序列。
- 不变量（debug 构建中每一步都检查）：best bid < best ask；每档 total_qty 等于队列中订单数量之和；索引中的订单数等于簿上的订单数。
- 差分测试没通过的版本，不做性能测量。
- 验证新版本 vN 时只测 vN：`LOB_DIFF_VERSIONS=vN`（debug ctest 和加大的差分测试都带上），没有改动的旧版本跳过。改了共享代码（types、events、replay/harness、generator、book_config、hier_bitmap、diff_runner 等）时不设这个变量，跑全部版本。

## Workload

- 生成器使用固定 seed，参数可配置：价格距最优价的分布（集中在前几档）、撤单比例（默认偏高，接近真实市场）、订单存活时间、买卖比例。
  - 实现说明：撤单由订单寿命驱动（M/M/∞，Little 定律使簿深度稳定）；撤单比例是**输出**（≈ 挂单 − 成交），通过 `--lifetime` 和 `--p-marketable` 调节。固定撤单概率会让簿被掏空或无限增长（实测见 `include/lob/workload/generator.hpp` 注释）。
- 生成一次后存为二进制文件；benchmark 时整体读入内存再回放，生成器的开销不计入测量。
- 之后：解析 NASDAQ TotalView-ITCH 5.0 样本数据，用真实数据回放。

## 实验日志

每个优化一条记录，失败或无效的优化也要记录：

```
## [日期] vN: <一句话标题>
- 假设：
- 改动：
- 结果（环境：vm/metal，commit：xxxxxxx）：指标 前 → 后
- 解释（引用 cachegrind / perf 数据）：
- 意外与遗留问题：
```
