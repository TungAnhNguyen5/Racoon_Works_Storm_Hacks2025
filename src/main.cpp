#include "parser.hpp"
#include "scheduler.hpp"
#include <fstream>
#include <iostream>

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

    std::cout << "Schedule (order):\n";
    for (size_t i = 0; i < result.execution_order.size(); ++i) {
        if (i) std::cout << " -> ";
        const auto& name = result.execution_order[i];
        bool rc = (i < result.recompute_flags.size()) ? result.recompute_flags[i] : false;
        if (rc) std::cout << name << "*"; else std::cout << name;
    }
    std::cout << "\n* denotes recomputation\n";
    std::cout << "Total time: " << result.total_time << "\n";
    std::cout << "Memory peak: " << result.memory_peak
              << " (limit=" << prob.total_memory << ")\n";
    return 0;
}
