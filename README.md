# Racoon Works — Storm Hacks 2025

**Custom Challenge #2: Memory-Constrained Graph Scheduling with Recomputation**

A C++17 scheduler for the Huawei Custom Challenge #2. Given a DAG of compute
operators with memory and time costs and a fixed memory budget, it produces an
execution order — with recomputation when needed — that fits the budget while
keeping total time low.

---

## Problem in one paragraph

Each node has `run_mem` (workspace during execution), `output_mem` (size of the
result kept in memory after execution), and `time_cost`. An output stays
resident until every uncomputed consumer has run. When pressure is high the
scheduler may **spill** a resident output and **recompute** it later. The peak
memory at each step is taken to be
`max(current + run_mem, current − freed_inputs + output_mem)` and the schedule
is feasible iff the running peak never exceeds the limit.

---

## Project layout

```
├── include/
│   ├── model.hpp        # Node, Problem, ScheduleState (all 64-bit memory)
│   ├── parser.hpp       # Input parsing API
│   └── scheduler.hpp    # Scheduler API + ExecuteOrder spec entry point
├── src/
│   ├── main.cpp         # CLI: reads input, dispatches, prints result
│   ├── parser.cpp       # Parses the example .txt format and a simple format
│   ├── scheduler.cpp    # memoryAwareGreedy, streamingTopological, ExecuteOrder
│   └── baseline.cpp     # Kahn-only topological reference (no memory mgmt)
├── input/               # example1.txt … example7.txt
├── docs/                # Challenge spec PDF
├── build.sh             # Convenience build script
└── CMakeLists.txt       # CMake configuration (g++ also works directly)
```

---

## Build

The project is plain C++17 with no external dependencies.

```bash
# Recommended: CMake
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . --config Release

# Or directly with g++
g++ -std=c++17 -O2 -Iinclude src/main.cpp src/scheduler.cpp src/parser.cpp \
    -o build/scheduler
```

Produces `build/scheduler` (and `build/baseline` via CMake).

---

## Usage

```bash
./build/scheduler input/example1.txt
./build/baseline  input/example1.txt
```

The scheduler prints the algorithm chosen, per-run statistics, the schedule,
total time, and the achieved memory peak versus the limit.

### Spec entry point

For embedding in another program, `ExecuteOrder` matches the signature in the
challenge document:

```cpp
#include "scheduler.hpp"

std::vector<Node> order = ExecuteOrder(all_nodes, output_name, total_memory);
```

It builds the `Problem`, dispatches by graph size, and returns the execution
sequence as a `std::vector<Node>` (recomputed nodes appear multiple times,
exactly as in the spec's worked example).

---

## Input format

**First line:** `Return <total_memory>` (bytes).

**Subsequent lines:**

```
<id> <name> <num_inputs> <input_id_1> ... <input_id_k> <run_mem> <output_mem> <time_cost>
```

A simpler `total_memory: <N>` + `node <name> <run_mem> <output_mem> <time_cost> inputs=<...>`
format is also accepted as a fallback.

---

## Algorithms

The scheduler dispatches on graph size:

### `memoryAwareGreedySchedule` — small / medium graphs (< 10 000 nodes)

Sticky-goal restoration with chain-aware protection:

1. Pick a forward goal (topologically-earliest uncomputed node, ties broken by
   smallest missing-input mass, then smallest per-node peak).
2. Build a restoration plan: ancestors whose outputs aren't currently
   resident, in topological order.
3. Walk the plan: pop the earliest entry whose own inputs are resident, spill
   non-protected outputs until it fits, run it as a recompute (no auto-free).
4. When all of the goal's inputs are resident, spill non-protected outputs
   until the goal fits and run it forward (auto-frees inputs whose only
   remaining consumer was the goal).

The **protected set** shrinks as restoration progresses so it never grows
unboundedly, and `garbageCollectOutputs` runs only at the start of each new
round to avoid tearing down chains mid-restoration.

### `streamingTopologicalSchedule` — large graphs (≥ 10 000 nodes)

A single-pass Kahn topological walk with reference-counted release:

- A min-heap ready queue ordered by `(output_mem, peak, time, name)` keeps the
  live working set compact.
- Each output is auto-freed when its remaining-consumer count hits zero.
- A lazy max-heap tracks resident outputs for spill candidates.
- **Just-in-time recompute**: if a popped node finds an input was previously
  spilled, `ensureResident` recursively re-materializes the missing ancestor
  chain. In-flight ancestors are protected from being re-spilled, and any
  ephemeral re-materializations are GC'd at the end of the step.

Per-step cost is amortized `O(log N)` plus the chain-restoration depth.

### `ExecuteOrder` — spec-style entry point

A thin wrapper that builds a `Problem` from `std::vector<Node>` and runs the
appropriate algorithm.

---

## Memory-peak model

The peak after running a node `B` from a resident set is computed as

```
peak_during_run = current_resident + run_mem(B)
peak_after_run  = current_resident − freed_input_bytes(B) + output_mem(B)
step_peak       = max(peak_during_run, peak_after_run)
memory_peak     = max(memory_peak, step_peak)
```

`freed_input_bytes(B)` = total `output_mem` of inputs of `B` whose only
remaining uncomputed consumer is `B` itself (so they get reclaimed after this
step). Recompute steps pass `freed = 0` because the chain must stay live.

All memory quantities are 64-bit (the largest test case has a 62 GB limit).

---

## Validation

All seven shipped examples produce feasible schedules:

| Example | Nodes   | Limit        | Peak         | Recomputes | Total time |
|---------|---------|--------------|--------------|------------|------------|
| 1       |     80  |  42.47 MB    |  42.21 MB    |      74    |     68 654 |
| 2       |     80  |  85.98 MB    |  69.99 MB    |       0    |     33 532 |
| 3       |     87  |  33.56 MB    |  33.55 MB    |      49    |     57 859 |
| 4       |     87  |  54.53 MB    |  50.86 MB    |       0    |     35 094 |
| 5       | 238 327 |  62.28 GB    |  62.28 GB    | 392 142    | 310 034 061|
| 6       | 238 327 |  20.70 GB    |  20.70 GB    | 667 277    | 444 693 357|
| 7       | 238 327 |  42.95 GB    |  42.95 GB    | 519 055    | 371 915 776|

Examples 5–7 share the same graph but have different memory budgets; the
recompute count (and therefore total time) scales with how tight the budget
is, exactly as the spec predicts.

---

## Output format

```
Problem: <N> nodes, memory limit <bytes>
Algorithm: <chosen scheduler>
<per-run stats line>
Schedule (order):
<name1> -> <name2> -> ... -> <nameK>
* denotes recomputation
Total time: <accumulated time_cost>
Memory peak: <bytes> (limit=<bytes>)
```

---

## Team — Racoon Works

| Member          | Email                       |
|-----------------|-----------------------------|
| Dennis Kritchko | dennis.kritchko@gmail.com   |
| Winston Thov    | winstonthov@gmail.com       |
| Muneeb Kamran   | muneebkamran04@gmail.com    |
| James Nguyen    | tunganhstu@gmail.com        |

---

## License

Developed for Storm Hacks 2025. See `docs/` for the full challenge brief.
