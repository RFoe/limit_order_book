# Static / simulated analysis without PEBS

The VM has no PEBS and no LBR, so per-instruction attribution of stalls and
branch misses is not available from the hardware. These tools fill part of the
gap with deterministic analysis of the **profile build** (`-march=x86-64-v3`,
valgrind-safe), which is also measured with hardware counters so model and
measurement refer to the same binary.

## 1. Dominant path → uiCA / llvm-mca (port pressure, issue width, dependencies)

```bash
B=build/profile/book; W=data/itch12302019_AAPL.ops
valgrind --tool=callgrind --dump-instr=yes --collect-jumps=yes --callgrind-out-file=cg.out \
    $B replay --workload $W --version v4
tools/static_path/extract.py cg.out $B --list-hot 'operator()<0ul'     # find loop head / op entries
tools/static_path/extract.py cg.out $B --start 0xENTRY --stop 0xLOOPHEAD -o path.s
as path.s -o path.o
~/git-repositories/uiCA/uiCA.py path.o -arch ICL                         # Ice Lake model
llvm-mca-23 -mcpu=icelake-client path.mca.s                               # cross-check
```

Addresses are specific to one build: re-run `--list-hot` after every rebuild.

uiCA setup (outside the repo): `git clone https://github.com/andreas-abel/uiCA`,
`./setup.sh` (builds XED, downloads uops.info data). Local patch needed with the
current XED: it names VEX GPR operands `VGPR*` while the uops.info data uses
`GPR*`, so mulx/shrx were "not supported"; `instructions.py` falls back to the
`GPR` name.

Caveats:
- The path is stitched from per-branch majorities (not a single real trace).
- Both tools assume all loads hit L1 and no branch mispredicts.
- jmp/call/ret are dropped and the path is laid out contiguously, so uiCA's
  front-end (it simulates legacy decode for non-loop code) is pessimistic;
  read the issue / port / dependency bounds for the back end.

## 2. Which branches mispredict

```bash
valgrind --tool=cachegrind --cache-sim=no --branch-sim=yes --instr-at-start=no \
    --cachegrind-out-file=bsim.out $B replay --workload $W --version v4
```

Per-source-line `Bcm`/`Bim` (mispredicted conditional/indirect branches) from a
simple simulated predictor. Data-dependent 50/50 branches are mispredicted by
both the simulator and real hardware; validate the total against the measured
`branch-misses` per op.
