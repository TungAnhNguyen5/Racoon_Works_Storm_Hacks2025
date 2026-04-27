#include "scheduler.hpp"

#include <algorithm>
#include <functional>
#include <iostream>
#include <limits>
#include <queue>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// =============================================================================
// Helpers — memory accounting per the spec's worked examples
// =============================================================================
//
// Spec model (Huawei Custom Challenge #2):
//   peak_during_run  = current_resident + run_mem
//   peak_after_run   = current_resident - freed_input_bytes + output_mem
//   step_peak        = max(peak_during_run, peak_after_run)
//   memory_peak      = max(memory_peak, step_peak)
//
// `freed_input_bytes` is the total output_mem of inputs whose only remaining
// uncomputed consumer is the node we are about to run (so the auto-free
// reclamation happens after this step). For a recomputation step we keep
// the chain alive, so callers pass freed = 0.

// Predict the step peak induced by running `node` next, given freed bytes.
static long long stepPeak(const ScheduleState& state, const Node& node, long long freed) {
    long long curr = state.current_memory;
    long long peak_during = curr + node.getRunMem();
    long long peak_after  = curr - freed + node.getOutputMem();
    if (peak_after < 0) peak_after = 0;
    long long step = std::max(peak_during, peak_after);
    return std::max(state.memory_peak, step);
}

// Compute the bytes that would be freed if `node_name` runs as a forward
// step from `state` (i.e. inputs whose only remaining uncomputed consumer
// is this node, excluding the node itself in the consumer count).
static long long predictFreedForward(const Problem& prob,
                                     const ScheduleState& state,
                                     const Node& node,
                                     const std::string& node_name) {
    long long freed = 0;
    for (const auto& input_name : node.getInputs()) {
        auto outIt = state.output_memory.find(input_name);
        if (outIt == state.output_memory.end()) continue;
        auto dep_it = prob.dependencies.find(input_name);
        bool all_done = true;
        if (dep_it != prob.dependencies.end()) {
            for (const auto& consumer : dep_it->second) {
                if (consumer == node_name) continue;
                if (state.computed.find(consumer) == state.computed.end()) {
                    all_done = false;
                    break;
                }
            }
        }
        if (all_done) freed += outIt->second;
    }
    return freed;
}

// Run `node_name` as a forward step in `state` and return the next state.
// Auto-frees inputs whose only remaining uncomputed consumer is this node.
static ScheduleState executeNode(const std::string& node_name,
                                 const Problem& prob,
                                 const ScheduleState& state) {
    ScheduleState next = state;
    const Node& node = prob.nodes.at(node_name);

    std::unordered_set<std::string> freeable;
    long long freed = 0;
    for (const auto& input_name : node.getInputs()) {
        auto outIt = state.output_memory.find(input_name);
        if (outIt == state.output_memory.end()) continue;
        auto dep_it = prob.dependencies.find(input_name);
        bool all_done = true;
        if (dep_it != prob.dependencies.end()) {
            for (const auto& consumer : dep_it->second) {
                if (consumer == node_name) continue;
                if (state.computed.find(consumer) == state.computed.end()) {
                    all_done = false;
                    break;
                }
            }
        }
        if (all_done) {
            freeable.insert(input_name);
            freed += outIt->second;
        }
    }

    next.memory_peak = stepPeak(state, node, freed);

    for (const auto& nm : freeable) next.output_memory.erase(nm);

    long long impact = node.getOutputMem() - freed;
    long long new_current = next.current_memory + impact;
    if (new_current < 0) new_current = 0;
    next.current_memory = new_current;
    next.total_time += node.getTimeCost();
    next.output_memory[node.getName()] = node.getOutputMem();
    next.execution_order.push_back(node.getName());

    bool isRecompute = (state.computed.count(node.getName()) > 0);
    next.recompute_flags.push_back(isRecompute);
    next.computed.insert(node.getName());
    return next;
}

// Drop resident outputs that no uncomputed consumer needs anymore.
static void garbageCollectOutputs(const Problem& prob, ScheduleState& state) {
    std::vector<std::string> toErase;
    for (const auto& kv : state.output_memory) {
        const std::string& name = kv.first;
        auto itSucc = prob.successors.find(name);
        bool needed = false;
        if (itSucc != prob.successors.end()) {
            for (const auto& cons : itSucc->second) {
                if (state.computed.find(cons) == state.computed.end()) {
                    needed = true;
                    break;
                }
            }
        }
        if (!needed) toErase.push_back(name);
    }
    for (const auto& name : toErase) {
        auto it = state.output_memory.find(name);
        if (it != state.output_memory.end()) {
            state.current_memory = std::max<long long>(0, state.current_memory - it->second);
            state.output_memory.erase(it);
        }
    }
}

// =============================================================================
// Memory-aware greedy with sticky-goal, chain-aware restoration
// =============================================================================
//
// Each round:
//   1. Pick a forward goal (topologically-earliest uncomputed node, ties
//      broken by smallest missing-input mass, then smallest peak).
//   2. Build a restoration plan: ancestors of the goal whose outputs aren't
//      currently resident, sorted in topological order.
//   3. While the goal isn't ready, pop the earliest plan entry whose own
//      inputs are resident; spill non-protected outputs until it fits;
//      run it as a recompute (no auto-free).
//   4. When the goal's inputs are all resident, spill non-protected outputs
//      until the goal fits and run it via executeNode (forward, allows
//      auto-free). End of round.
//
// Protected set per iteration =
//      goal ∪ resident-inputs-of-goal
//           ∪ resident-inputs-of-(remaining plan entries)
//           ∪ target ∪ resident-inputs-of-target
// Once a restored output is no longer needed by any remaining plan step
// or by the goal, it becomes spillable again — preventing the protected
// set from accumulating unboundedly across long restoration chains.
ScheduleState memoryAwareGreedySchedule(const Problem& prob) {
    ScheduleState cur;
    const size_t N = prob.nodes.size();
    if (N == 0) return cur;
    const size_t actionBudget = 100 * N + 1000;

    auto allInputsResident = [&](const Node& n) {
        for (const auto& in : n.getInputs()) {
            if (cur.output_memory.find(in) == cur.output_memory.end()) return false;
        }
        return true;
    };

    // Topological depth: source nodes are 0; deeper nodes get larger values.
    std::unordered_map<std::string, int> depth;
    {
        std::function<int(const std::string&)> dfs = [&](const std::string& n) -> int {
            auto it = depth.find(n);
            if (it != depth.end()) return it->second;
            depth[n] = 0; // cycle guard
            int d = 0;
            auto itN = prob.nodes.find(n);
            if (itN != prob.nodes.end()) {
                for (const auto& in : itN->second.getInputs()) {
                    d = std::max(d, dfs(in) + 1);
                }
            }
            depth[n] = d;
            return d;
        };
        for (const auto& kv : prob.nodes) dfs(kv.first);
    }

    auto pickGoal = [&]() -> std::string {
        std::string best;
        int bestDepth = std::numeric_limits<int>::max();
        long long bestMissing = std::numeric_limits<long long>::max();
        long long bestPeak = std::numeric_limits<long long>::max();
        for (const auto& kv : prob.nodes) {
            const std::string& name = kv.first;
            if (cur.computed.count(name)) continue;
            const Node& n = kv.second;
            int d = depth[name];
            long long missing = 0;
            for (const auto& in : n.getInputs()) {
                if (cur.output_memory.find(in) == cur.output_memory.end()) {
                    auto it = prob.nodes.find(in);
                    if (it != prob.nodes.end()) missing += it->second.getOutputMem();
                }
            }
            long long p = n.getPeak();
            if (d < bestDepth
                || (d == bestDepth && missing < bestMissing)
                || (d == bestDepth && missing == bestMissing && p < bestPeak)) {
                bestDepth = d;
                bestMissing = missing;
                bestPeak = p;
                best = name;
            }
        }
        return best;
    };

    auto buildRestorationPlan = [&](const std::string& goal) -> std::vector<std::string> {
        std::unordered_set<std::string> needed;
        std::vector<std::string> work;
        auto itG = prob.nodes.find(goal);
        if (itG == prob.nodes.end()) return {};
        for (const auto& in : itG->second.getInputs()) {
            if (!cur.output_memory.count(in) && !needed.count(in)) {
                needed.insert(in);
                work.push_back(in);
            }
        }
        while (!work.empty()) {
            std::string n = work.back();
            work.pop_back();
            auto it = prob.nodes.find(n);
            if (it == prob.nodes.end()) continue;
            for (const auto& in : it->second.getInputs()) {
                if (!cur.output_memory.count(in) && !needed.count(in)) {
                    needed.insert(in);
                    work.push_back(in);
                }
            }
        }
        std::vector<std::string> plan(needed.begin(), needed.end());
        std::sort(plan.begin(), plan.end(), [&](const std::string& a, const std::string& b) {
            int da = depth[a], db = depth[b];
            if (da != db) return da < db;
            return a < b;
        });
        return plan;
    };

    auto buildProtectedSet = [&](const std::string& goal,
                                 const std::vector<std::string>& plan,
                                 const std::string& target) {
        std::unordered_set<std::string> p;
        p.insert(goal);
        const Node& gn = prob.nodes.at(goal);
        for (const auto& in : gn.getInputs()) {
            if (cur.output_memory.count(in)) p.insert(in);
        }
        for (const auto& nm : plan) {
            auto it = prob.nodes.find(nm);
            if (it == prob.nodes.end()) continue;
            for (const auto& in : it->second.getInputs()) {
                if (cur.output_memory.count(in)) p.insert(in);
            }
        }
        if (!target.empty()) {
            p.insert(target);
            const Node& tn = prob.nodes.at(target);
            for (const auto& in : tn.getInputs()) {
                if (cur.output_memory.count(in)) p.insert(in);
            }
        }
        return p;
    };

    auto spillOne = [&](const std::unordered_set<std::string>& protectedSet) {
        // Prefer dead outputs (no remaining uncomputed consumer) first.
        for (const auto& kv : cur.output_memory) {
            if (protectedSet.count(kv.first)) continue;
            auto itS = prob.successors.find(kv.first);
            bool needed = false;
            if (itS != prob.successors.end()) {
                for (const auto& cons : itS->second) {
                    if (!cur.computed.count(cons)) { needed = true; break; }
                }
            }
            if (!needed) {
                cur.current_memory = std::max<long long>(0, cur.current_memory - kv.second);
                cur.output_memory.erase(kv.first);
                return true;
            }
        }
        // Otherwise spill the largest non-protected output.
        std::string bestName;
        long long bestSize = -1;
        for (const auto& kv : cur.output_memory) {
            if (protectedSet.count(kv.first)) continue;
            if (kv.second > bestSize) {
                bestSize = kv.second;
                bestName = kv.first;
            }
        }
        if (bestName.empty()) return false;
        cur.current_memory = std::max<long long>(0, cur.current_memory - bestSize);
        cur.output_memory.erase(bestName);
        return true;
    };

    // Recompute step: add output, do NOT auto-free inputs (would tear down
    // the chain we just rebuilt). Caller must ensure peak fits.
    auto runRecompute = [&](const std::string& name) {
        const Node& n = prob.nodes.at(name);
        cur.memory_peak = stepPeak(cur, n, /*freed=*/0);
        cur.current_memory += n.getOutputMem();
        cur.total_time += n.getTimeCost();
        cur.output_memory[name] = n.getOutputMem();
        cur.execution_order.push_back(name);
        cur.recompute_flags.push_back(true);
        cur.computed.insert(name);
    };

    std::string goal;
    std::vector<std::string> plan;

    size_t actions = 0;
    size_t executes = 0, recomputes = 0, spills = 0;
    while (cur.computed.size() < N && actions++ < actionBudget) {
        if (goal.empty() || cur.computed.count(goal)) {
            garbageCollectOutputs(prob, cur);
            goal = pickGoal();
            if (goal.empty()) break;
            plan = buildRestorationPlan(goal);
        }

        const Node& gn = prob.nodes.at(goal);
        if (gn.getPeak() > prob.total_memory) {
            std::cerr << "memAwareGreedy: goal " << goal
                      << " peak alone exceeds limit ("
                      << gn.getPeak() << " > " << prob.total_memory << ")\n";
            break;
        }

        if (allInputsResident(gn)) {
            std::unordered_set<std::string> protectedSet = buildProtectedSet(goal, plan, "");
            bool stuck = false;
            while (true) {
                long long freed = predictFreedForward(prob, cur, gn, goal);
                if (stepPeak(cur, gn, freed) <= prob.total_memory) break;
                if (!spillOne(protectedSet)) { stuck = true; break; }
                ++spills;
            }
            if (stuck) {
                std::cerr << "memAwareGreedy: cannot fit goal " << goal
                          << " (curMem=" << cur.current_memory
                          << " peak=" << gn.getPeak()
                          << " limit=" << prob.total_memory << ")\n";
                break;
            }
            cur = executeNode(goal, prob, cur);
            ++executes;
            goal.clear();
            plan.clear();
            continue;
        }

        // Find the earliest plan entry whose inputs are resident.
        size_t chosenIdx = plan.size();
        for (size_t i = 0; i < plan.size(); ++i) {
            auto itN = prob.nodes.find(plan[i]);
            if (itN == prob.nodes.end()) continue;
            if (allInputsResident(itN->second)) { chosenIdx = i; break; }
        }
        if (chosenIdx == plan.size()) {
            std::cerr << "memAwareGreedy: no recompute candidate ready for goal "
                      << goal << " (plan size=" << plan.size()
                      << " computed=" << cur.computed.size() << "/" << N << ")\n";
            break;
        }
        std::string target = plan[chosenIdx];
        const Node& tn = prob.nodes.at(target);

        std::vector<std::string> remainingPlan;
        remainingPlan.reserve(plan.size() - 1);
        for (size_t i = 0; i < plan.size(); ++i) {
            if (i != chosenIdx) remainingPlan.push_back(plan[i]);
        }
        std::unordered_set<std::string> protectedSet =
            buildProtectedSet(goal, remainingPlan, target);

        bool stuck = false;
        while (stepPeak(cur, tn, /*freed=*/0) > prob.total_memory) {
            if (!spillOne(protectedSet)) { stuck = true; break; }
            ++spills;
        }
        if (stuck) {
            std::cerr << "memAwareGreedy: cannot fit recompute target " << target
                      << " for goal " << goal
                      << " (curMem=" << cur.current_memory
                      << " runMem=" << tn.getRunMem()
                      << " outMem=" << tn.getOutputMem() << ")\n";
            break;
        }
        runRecompute(target);
        plan = std::move(remainingPlan);
        ++recomputes;
    }

    std::cerr << "memAwareGreedy: actions=" << actions
              << " executes=" << executes
              << " recomputes=" << recomputes
              << " spills=" << spills
              << " computed=" << cur.computed.size() << "/" << N
              << " peak=" << cur.memory_peak
              << " limit=" << prob.total_memory << "\n";
    return cur;
}

// =============================================================================
// Streaming topological greedy (large-graph path)
// =============================================================================
//
// Designed for graphs in the hundreds-of-thousands of nodes range where the
// O(N^2)-ish per-step scans of memoryAwareGreedySchedule are too slow.
//
//   - Indegree-driven topological execution via a min-heap ready queue.
//   - Per-node remaining-consumer counter; when it hits zero the output is
//     released immediately (no GC pass needed).
//   - On memory pressure, spill candidates come from a lazy max-heap of
//     resident outputs ordered by size; we skip stale entries (already
//     freed/spilled) on demand. Only the current node's inputs need to be
//     "protected" from spilling, which is a small constant-size check.
//   - No transitive chain analysis, no global protected-set construction.
//   - Per-step cost is amortized O(log N).
//
// Just-in-time recompute: when the next node finds a previously-spilled
// input missing, we recursively re-materialize the missing ancestor chain
// (`ensureResident`). The chain protects itself via an `inflight` set so
// the spill loop doesn't evict in-progress restorations. Ephemeral
// re-materializations are GC'd at the end of each main-loop step.
ScheduleState streamingTopologicalSchedule(const Problem& prob) {
    ScheduleState cur;
    const size_t N = prob.nodes.size();
    if (N == 0) return cur;

    std::unordered_map<std::string, int> indeg;
    std::unordered_map<std::string, int> rc; // remaining uncomputed consumers
    indeg.reserve(N * 2);
    rc.reserve(N * 2);
    for (const auto& kv : prob.nodes) {
        indeg[kv.first] = static_cast<int>(kv.second.getInputs().size());
        auto sit = prob.successors.find(kv.first);
        rc[kv.first] = (sit != prob.successors.end())
            ? static_cast<int>(sit->second.size())
            : 0;
    }

    // Ready queue: smallest output_mem first, ties on peak then time. This
    // keeps the live working set as compact as possible while we stream.
    using Cand = std::tuple<long long, long long, int, std::string>;
    auto cmp = [](const Cand& a, const Cand& b) {
        if (std::get<0>(a) != std::get<0>(b)) return std::get<0>(a) > std::get<0>(b);
        if (std::get<1>(a) != std::get<1>(b)) return std::get<1>(a) > std::get<1>(b);
        if (std::get<2>(a) != std::get<2>(b)) return std::get<2>(a) > std::get<2>(b);
        return std::get<3>(a) > std::get<3>(b);
    };
    std::priority_queue<Cand, std::vector<Cand>, decltype(cmp)> ready(cmp);

    auto pushReady = [&](const std::string& name) {
        const Node& n = prob.nodes.at(name);
        ready.emplace(n.getOutputMem(), n.getPeak(), n.getTimeCost(), name);
    };

    for (const auto& kv : indeg) {
        if (kv.second == 0) pushReady(kv.first);
    }

    // Lazy max-heap of resident outputs by size. Entries can become stale
    // (output was freed/spilled); we skip those when popping.
    using Resid = std::pair<long long, std::string>; // (size, name)
    std::priority_queue<Resid> residentBySize;

    auto freeOutput = [&](const std::string& name) {
        auto it = cur.output_memory.find(name);
        if (it == cur.output_memory.end()) return;
        cur.current_memory = std::max<long long>(0, cur.current_memory - it->second);
        cur.output_memory.erase(it);
    };

    size_t spills = 0;
    size_t recomputes = 0;
    size_t failedFit = 0;

    // Just-in-time chain restoration. `inflight` and `ephemerals` are
    // shared across the recursive calls; each main-loop step starts with
    // both empty and ends with `ephemerals` GC'd if their rc is now zero.
    std::unordered_set<std::string> inflight;
    std::vector<std::string> ephemerals;

    auto spillToFit = [&](const Node& n,
                          const std::unordered_set<std::string>& protectedSet,
                          long long freedHint) -> bool {
        std::vector<Resid> reinsert;
        long long predicted = stepPeak(cur, n, freedHint);
        while (predicted > prob.total_memory) {
            std::string spillName;
            long long spillSize = -1;
            while (!residentBySize.empty()) {
                Resid r = residentBySize.top();
                residentBySize.pop();
                auto rit = cur.output_memory.find(r.second);
                if (rit == cur.output_memory.end() || rit->second != r.first) continue;
                if (protectedSet.count(r.second)) { reinsert.push_back(r); continue; }
                spillName = r.second;
                spillSize = r.first;
                break;
            }
            if (spillName.empty()) break;
            cur.current_memory = std::max<long long>(0, cur.current_memory - spillSize);
            cur.output_memory.erase(spillName);
            ++spills;
            predicted = stepPeak(cur, n, freedHint);
        }
        for (auto& r : reinsert) residentBySize.push(r);
        return predicted <= prob.total_memory;
    };

    std::function<bool(const std::string&)> ensureResident =
        [&](const std::string& name) -> bool {
            if (cur.output_memory.count(name)) return true;
            auto nIt = prob.nodes.find(name);
            if (nIt == prob.nodes.end()) return false;
            const Node& n = nIt->second;

            inflight.insert(name);
            for (const auto& in : n.getInputs()) {
                if (!ensureResident(in)) { inflight.erase(name); return false; }
            }

            std::unordered_set<std::string> protectedSet = inflight;
            for (const auto& in : n.getInputs()) protectedSet.insert(in);
            if (!spillToFit(n, protectedSet, /*freedHint=*/0)) {
                inflight.erase(name);
                return false;
            }

            cur.memory_peak = stepPeak(cur, n, /*freed=*/0);
            cur.current_memory += n.getOutputMem();
            cur.total_time += n.getTimeCost();
            cur.output_memory[name] = n.getOutputMem();
            residentBySize.emplace(n.getOutputMem(), name);
            cur.execution_order.push_back(name);
            cur.recompute_flags.push_back(true);
            ephemerals.push_back(name);
            ++recomputes;
            inflight.erase(name);
            return true;
        };

    while (!ready.empty()) {
        Cand top = ready.top(); ready.pop();
        const std::string& name = std::get<3>(top);
        if (cur.computed.count(name)) continue;

        const Node& n = prob.nodes.at(name);

        ephemerals.clear();
        bool restoredOk = true;
        for (const auto& in : n.getInputs()) {
            if (cur.output_memory.count(in)) continue;
            if (!ensureResident(in)) { restoredOk = false; break; }
        }
        if (!restoredOk) {
            std::cerr << "streamTop: cannot restore inputs for " << name << "\n";
            ++failedFit;
            break;
        }

        std::unordered_set<std::string> protectedInputs(
            n.getInputs().begin(), n.getInputs().end());

        // Predict freed bytes: inputs whose remaining-consumer count is 1
        // (this run is the last) and that are currently resident.
        auto predictFreedStreaming = [&]() -> long long {
            long long freed = 0;
            for (const auto& in : n.getInputs()) {
                auto outIt = cur.output_memory.find(in);
                if (outIt == cur.output_memory.end()) continue;
                auto rcIt = rc.find(in);
                if (rcIt != rc.end() && rcIt->second == 1) freed += outIt->second;
            }
            return freed;
        };

        long long freedNow = predictFreedStreaming();
        if (!spillToFit(n, protectedInputs, freedNow)) {
            std::cerr << "streamTop: cannot fit " << name
                      << " (curMem=" << cur.current_memory
                      << " runMem=" << n.getRunMem()
                      << " outMem=" << n.getOutputMem()
                      << " limit=" << prob.total_memory << ")\n";
            ++failedFit;
            break;
        }

        freedNow = predictFreedStreaming();
        cur.memory_peak = stepPeak(cur, n, freedNow);
        cur.current_memory += n.getOutputMem();
        cur.total_time += n.getTimeCost();
        cur.output_memory[name] = n.getOutputMem();
        residentBySize.emplace(n.getOutputMem(), name);
        cur.execution_order.push_back(name);
        cur.recompute_flags.push_back(false);
        cur.computed.insert(name);

        for (const auto& in : n.getInputs()) {
            auto rcIt = rc.find(in);
            if (rcIt == rc.end()) continue;
            if (--rcIt->second == 0) freeOutput(in);
        }

        auto sit = prob.successors.find(name);
        if (sit != prob.successors.end()) {
            for (const auto& succ : sit->second) {
                auto inIt = indeg.find(succ);
                if (inIt == indeg.end()) continue;
                if (--inIt->second == 0) pushReady(succ);
            }
        }

        // GC ephemerals brought back during this step but no longer needed.
        for (const auto& e : ephemerals) {
            auto rcIt = rc.find(e);
            if (rcIt != rc.end() && rcIt->second > 0) continue;
            freeOutput(e);
        }
    }

    std::cerr << "streamTop: computed=" << cur.computed.size() << "/" << N
              << " spills=" << spills
              << " recomputes=" << recomputes
              << " failedFit=" << failedFit
              << " peak=" << cur.memory_peak
              << " limit=" << prob.total_memory << "\n";
    return cur;
}

// =============================================================================
// ExecuteOrder — spec entry point
// =============================================================================
std::vector<Node> ExecuteOrder(const std::vector<Node>& all_nodes,
                               const std::string& output_name,
                               long long total_memory) {
    Problem prob;
    prob.total_memory = total_memory;
    for (const auto& n : all_nodes) prob.nodes.emplace(n.getName(), n);
    for (const auto& n : all_nodes) {
        for (const auto& in : n.getInputs()) {
            prob.dependencies[in].insert(n.getName());
            prob.successors[in].push_back(n.getName());
        }
        prob.successors.try_emplace(n.getName());
    }

    ScheduleState result;
    constexpr size_t kStreamingThreshold = 10000;
    if (all_nodes.size() >= kStreamingThreshold) {
        result = streamingTopologicalSchedule(prob);
        if (result.computed.size() != prob.nodes.size()) {
            result = memoryAwareGreedySchedule(prob);
        }
    } else {
        result = memoryAwareGreedySchedule(prob);
    }

    std::vector<Node> out;
    out.reserve(result.execution_order.size());
    for (const auto& name : result.execution_order) {
        auto it = prob.nodes.find(name);
        if (it != prob.nodes.end()) out.push_back(it->second);
    }
    (void)output_name; // sink is implicit in the DAG topology
    return out;
}
