#include "parser.hpp"
#include "scheduler.hpp"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

// Independently re-simulate a schedule using the spec's memory model.
//
// Per the spec's worked example, each instance of a node X (forward or
// recompute) lives in memory until the LAST consumer step of that instance,
// where "consumer step" is any step in (k_i, k_{i+1}) that has X in its
// inputs list — k_i is the step that produced the current instance and
// k_{i+1} is the next step in the schedule that recomputes X (or end of
// schedule). After that consumer step's end-of-step reclamation, X is freed.
// If an instance has zero consumers in its window, it is freed immediately
// at end of its own producing step (orphan / immediately-dead output).
//
// Step peak follows the spec model:
//   peak_during = curr + run_mem
//   peak_after  = curr - freed_input_bytes + output_mem
//   step_peak   = max(peak_during, peak_after)
struct VerifyResult {
    bool ok{true};
    long long total_time{0};
    long long memory_peak{0};
    std::string first_error;
    long long peak_step_index{-1};
    std::string peak_step_name;
    bool peak_was_recompute{false};
};

VerifyResult verifySchedule(const Problem& prob,
                            const std::vector<std::string>& order,
                            const std::vector<bool>& recompute_flags) {
    VerifyResult vr;

    // Pre-compute, for each step i, the set of input-names that should be
    // freed AT END of step i (i.e. step i is the LAST consumer of the
    // current instance of that input). Also pre-compute, for each step i,
    // whether the just-produced output has zero consumers in its instance
    // window — in that case we free it immediately at end of step i.

    // For each name X, all schedule positions where it is produced (forward
    // or recompute):
    std::unordered_map<std::string, std::vector<size_t>> producePositions;
    // All schedule positions where X is consumed (i.e. appears in
    // node[j].getInputs()):
    std::unordered_map<std::string, std::vector<size_t>> usePositions;

    for (size_t j = 0; j < order.size(); ++j) {
        producePositions[order[j]].push_back(j);
        auto nIt = prob.nodes.find(order[j]);
        if (nIt == prob.nodes.end()) continue;
        for (const auto& in : nIt->second.getInputs()) {
            usePositions[in].push_back(j);
        }
    }

    // freeAtEndOfStep[i] = list of names to free after step i finishes.
    std::vector<std::vector<std::string>> freeAtEndOfStep(order.size());
    for (auto& kv : producePositions) {
        const std::string& name = kv.first;
        const std::vector<size_t>& produces = kv.second;
        const std::vector<size_t>& uses = usePositions[name];
        for (size_t pi = 0; pi < produces.size(); ++pi) {
            size_t startStep = produces[pi];
            size_t endExcl = (pi + 1 < produces.size()) ? produces[pi + 1] : order.size();
            // Find last use j in (startStep, endExcl).
            // uses is sorted; use binary search.
            auto lo = std::upper_bound(uses.begin(), uses.end(), startStep);
            auto hi = std::lower_bound(uses.begin(), uses.end(), endExcl);
            if (lo == hi) {
                // No consumer in this instance's window: free at end of
                // start step itself (orphan/dead instance).
                freeAtEndOfStep[startStep].push_back(name);
            } else {
                size_t lastUse = *(hi - 1);
                freeAtEndOfStep[lastUse].push_back(name);
            }
        }
    }

    long long curr = 0;
    std::unordered_map<std::string, long long> resident;

    for (size_t i = 0; i < order.size(); ++i) {
        const std::string& name = order[i];
        bool isRecompute = (i < recompute_flags.size() && recompute_flags[i]);
        auto nIt = prob.nodes.find(name);
        if (nIt == prob.nodes.end()) {
            vr.ok = false;
            vr.first_error = "unknown node in schedule: " + name;
            return vr;
        }
        const Node& n = nIt->second;

        for (const auto& in : n.getInputs()) {
            if (!resident.count(in)) {
                vr.ok = false;
                vr.first_error = "input " + in + " not resident when running "
                                  + name + " (step " + std::to_string(i) + ")";
                return vr;
            }
        }

        // freed bytes = sum of input output_mems whose lifetime ends here.
        // We treat name itself separately so don't include `name` in freed.
        long long freed = 0;
        std::unordered_set<std::string> freeSet(
            freeAtEndOfStep[i].begin(), freeAtEndOfStep[i].end());
        for (const auto& in : n.getInputs()) {
            auto rIt = resident.find(in);
            if (rIt == resident.end()) continue;
            if (freeSet.count(in)) freed += rIt->second;
        }

        long long peak_during = curr + n.getRunMem();
        long long peak_after  = curr - freed + n.getOutputMem();
        if (peak_after < 0) peak_after = 0;
        long long step_peak = std::max(peak_during, peak_after);
        if (step_peak > vr.memory_peak) {
            vr.memory_peak = step_peak;
            vr.peak_step_index = static_cast<long long>(i);
            vr.peak_step_name = name;
            vr.peak_was_recompute = isRecompute;
        }

        // Apply reclamations scheduled to occur at end of this step.
        for (const auto& fnm : freeAtEndOfStep[i]) {
            if (fnm == name) continue; // handled below for fresh write
            auto rIt = resident.find(fnm);
            if (rIt != resident.end()) {
                curr -= rIt->second;
                resident.erase(rIt);
            }
        }
        // Recompute writes a fresh copy under the same name: drop the old
        // resident bytes (if any) before adding the new output_mem.
        if (resident.count(name)) {
            curr -= resident[name];
            resident.erase(name);
        }
        curr += n.getOutputMem();
        resident[name] = n.getOutputMem();
        // If this instance is itself orphan/dead, freeAtEndOfStep[i]
        // contains `name`; release it immediately.
        if (freeSet.count(name)) {
            curr -= n.getOutputMem();
            resident.erase(name);
        }
        vr.total_time += n.getTimeCost();
    }
    return vr;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cout << "Usage: scheduler <input_file>\n";
        return 0;
    }
    std::ifstream fin(argv[1]);
    if (!fin) {
        std::cerr << "Failed to open input: " << argv[1] << "\n";
        return 1;
    }
    long long total_memory; std::vector<ParsedNodeSpec> specs; std::string error;
    if (!parseExamplesFormat(fin, total_memory, specs, error)) {
        fin.clear(); fin.seekg(0);
        if (!parseSimpleFormat(fin, total_memory, specs, error)) {
            std::cerr << "Parse error: " << error << "\n";
            return 2;
        }
    }
    Problem prob = buildProblem(total_memory, specs);

    std::cout << "Problem: " << prob.nodes.size() << " nodes, memory limit "
              << prob.total_memory << "\n";

    // Dispatch by graph size: small/medium graphs use the chain-aware
    // memory-aware greedy (handles tight memory via spill+recompute);
    // very large graphs use the streaming topological scheduler (single
    // pass, ref-counted release, lazy spill heap — no O(N) per-step scans).
    ScheduleState result;
    constexpr size_t kStreamingThreshold = 10000;
    if (prob.nodes.size() >= kStreamingThreshold) {
        std::cout << "Algorithm: streaming topological greedy\n";
        result = streamingTopologicalSchedule(prob);
        if (result.computed.size() != prob.nodes.size()) {
            std::cerr << "Streaming scheduler did not complete; "
                      << "falling back to memory-aware greedy.\n";
            result = memoryAwareGreedySchedule(prob);
        }
    } else {
        std::cout << "Algorithm: memory-aware greedy (sticky-goal restoration)\n";
        result = memoryAwareGreedySchedule(prob);
    }

    if (result.computed.size() != prob.nodes.size()) {
        std::cerr << "No feasible schedule found (computed "
                  << result.computed.size() << "/" << prob.nodes.size() << ").\n";
        return 3;
    }

    VerifyResult vr = verifySchedule(prob, result.execution_order, result.recompute_flags);
    if (!vr.ok) {
        std::cerr << "VERIFY FAIL: " << vr.first_error << "\n";
        return 4;
    }
    if (vr.total_time != result.total_time) {
        std::cerr << "TIME MISMATCH: scheduler reports " << result.total_time
                  << " vs verifier " << vr.total_time << "\n";
        return 5;
    }
    if (vr.memory_peak > prob.total_memory) {
        std::cerr << "OVER-LIMIT: verified peak " << vr.memory_peak
                  << " > limit " << prob.total_memory << "\n";
        return 6;
    }

    long long gap = prob.total_memory - vr.memory_peak;
    std::cout << "\n* denotes recomputation\n";
    std::cout << "Total time:  " << result.total_time << "\n";
    std::cout << "Memory peak: " << vr.memory_peak
              << " (limit=" << prob.total_memory
              << ", headroom=" << gap << " bytes)\n";
    std::cout << "  scheduler internal estimate (conservative): "
              << result.memory_peak << "\n";
    std::cout << "  peak step: " << vr.peak_step_index
              << " (" << vr.peak_step_name
              << (vr.peak_was_recompute ? ", RECOMPUTE" : ", forward") << ")\n";
    return 0;
}
