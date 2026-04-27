#include "parser.hpp"
#include <queue>
#include <iostream>
#include <fstream>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cout << "Usage: baseline <input_file>\n";
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

    // Kahn's algorithm for a simple topological order
    std::unordered_map<std::string, int> indeg;
    for (const auto& kv : prob.nodes) indeg[kv.first] = 0;
    for (const auto& kv : prob.nodes) {
        for (const auto& in : kv.second.getInputs()) indeg[kv.first]++;
    }
    std::queue<std::string> q;
    for (const auto& kv : indeg) if (kv.second == 0) q.push(kv.first);

    std::vector<std::string> order;
    order.reserve(prob.nodes.size());
    long total_time = 0;
    long long memory_peak = 0;
    long long current_memory = 0;

    while (!q.empty()) {
        std::string u = q.front(); q.pop();
        order.push_back(u);
        const Node& n = prob.nodes.at(u);
        total_time += n.getTimeCost();
        // naive memory accounting ignoring ceiling: add output, don't free inputs
        current_memory += n.getOutputMem();
        if (current_memory > memory_peak) memory_peak = current_memory;

        for (const auto& v : prob.successors[u]) {
            if (--indeg[v] == 0) q.push(v);
        }
    }

    if (order.size() != prob.nodes.size()) {
        std::cerr << "Graph has cycles or missing sources; cannot produce baseline.\n";
        return 3;
    }

    std::cout << "Baseline schedule (topological):\n";
    for (size_t i = 0; i < order.size(); ++i) {
        if (i) std::cout << " -> ";
        std::cout << order[i];
    }
    std::cout << "\nTotal time: " << total_time << "\n";
    std::cout << "Naive memory peak (no freeing): " << memory_peak << "\n";
    return 0;
}


