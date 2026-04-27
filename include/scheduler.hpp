#pragma once

#include "model.hpp"

// Memory-aware greedy scheduler with sticky-goal, chain-aware restoration.
//
// Picks a forward goal (topologically-earliest uncomputed node), pre-computes
// a restoration plan (ancestors whose outputs are missing), and walks it
// while spilling and recomputing under the memory limit. Returns the final
// ScheduleState; the schedule is feasible iff state.computed.size() equals
// prob.nodes.size(). Suitable for small/medium problems (O(N^2)-ish).
ScheduleState memoryAwareGreedySchedule(const Problem& prob);

// Streaming topological scheduler for large graphs.
//
// Single forward pass in topological order using a min-heap ready queue.
// Each output is freed automatically when its remaining-consumer count hits
// zero (reference-counted release). When the next node's predicted peak
// exceeds the limit, spills the largest non-input resident output via a
// lazy max-heap. No protected-set scans; per-step cost is O(log N).
ScheduleState streamingTopologicalSchedule(const Problem& prob);

// Spec-style entry point (Huawei Custom Challenge #2):
//
//   std::vector<Node> ExecuteOrder(const std::vector<Node>& all_nodes,
//                                  const std::string& output_name,
//                                  long total_memory)
//
// Builds a Problem from `all_nodes`, dispatches to the appropriate scheduler
// based on graph size, and returns the execution sequence as a vector of
// Node values (recomputed nodes appear multiple times in the order, exactly
// as the spec example shows). The caller can read `output_name` from the
// returned vector's last element (the schedule is built so that the sink is
// the final element when the graph terminates at a single output).
std::vector<Node> ExecuteOrder(const std::vector<Node>& all_nodes,
                               const std::string& output_name,
                               long long total_memory);
