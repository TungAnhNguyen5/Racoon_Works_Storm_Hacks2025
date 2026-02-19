# Racoon Works — Storm Hacks 2025

**Custom Challenge #2: Algorithm Reoptimization**

A C++ solver for memory-constrained computation graph scheduling. Given a DAG of compute nodes with memory and time costs, it produces an execution order (with optional recomputation) that minimizes total time while respecting a memory limit.

---

## Overview

This project addresses the **Huawei Custom Challenge #2** from Storm Hacks 2025. The goal is to schedule nodes in a computation graph so that:

- All dependencies are respected (topological order)
- Memory usage never exceeds the given limit
- Total execution time is minimized
- Recomputation is allowed when freeing memory helps reduce peak usage

The solver uses several strategies (greedy, heuristic search, beam search, etc.) and adapts parameters based on problem size.

---

## Project Structure

```
├── CMakeLists.txt      # CMake build configuration
├── build.sh            # Convenience build script
├── include/
│   ├── model.hpp       # Node, Problem, ScheduleState definitions
│   ├── parser.hpp      # Input parsing
│   └── scheduler.hpp   # Scheduling algorithms
├── src/
│   ├── main.cpp        # Scheduler entry point
│   ├── scheduler.cpp   # Core scheduling logic
│   ├── parser.cpp      # Parser implementation
│   └── baseline.cpp    # Baseline topological scheduler
├── input/              # Example input files (example1.txt – example7.txt)
└── docs/               # Challenge description and references
```

---

## Requirements

- **C++17** compiler (GCC or Clang)
- **CMake** 3.16+
- **Gurobi** 11.0+ (for the main scheduler)

> **Note:** The `scheduler` executable depends on Gurobi. By default, `GUROBI_HOME` is set to `/opt/gurobi1100/linux64`. Override via CMake if your installation path differs.

The **baseline** executable does not require Gurobi.

---

## Build

```bash
./build.sh
```

Or manually:

```bash
mkdir -p build
cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . --config Release
```

Executables are produced in `build/`: `scheduler` and `baseline`.

---

## Usage

### Scheduler (optimized)

```bash
./build/scheduler <input_file>
```

Example:

```bash
./build/scheduler input/example1.txt
```

### Baseline (topological order, no optimization)

```bash
./build/baseline <input_file>
```

Example:

```bash
./build/baseline input/example1.txt
```

---

## Input Format

**First line:** Total memory limit (bytes)

**Subsequent lines:** One node per line:

```
<index> <name> <num_inputs> <input_indices...> <run_mem> <output_mem> <time_cost>
```

- `index`: Node ID
- `name`: Node label
- `num_inputs`: Number of predecessor nodes
- `input_indices`: Indices of predecessor nodes
- `run_mem`: Memory during execution
- `output_mem`: Memory held after execution (output tensor)
- `time_cost`: Execution time

---

## Output Format

The scheduler prints:

- Execution order (topological)
- Recomputation markers (`*` for recomputed nodes)
- Total time
- Memory peak vs limit

---

## Algorithms

- **Greedy**: Fast heuristic suitable for large graphs
- **Heuristic schedule**: Rule-based prioritization
- **Main algorithm** (`scheduleWithDebug`): Search-based with configurable limits
- **Baseline**: Kahn's topological sort (reference only)

Parameters (e.g. max expansions, time limit) are scaled automatically by problem size.

---

## Team — Racoon Works

| Member        | Email                      |
|---------------|----------------------------|
| Dennis Kritchko | dennis.kritchko@gmail.com |
| Winston Thov  | winstonthov@gmail.com      |
| Muneeb Kamran | muneebkamran04@gmail.com   |
| James Nguyen  | tunganhstu@gmail.com       |

---

## License

This project was developed for Storm Hacks 2025. See the challenge materials in `docs/` for more context.
