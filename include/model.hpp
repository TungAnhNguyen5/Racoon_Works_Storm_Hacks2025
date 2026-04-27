#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>

class Node {
private:
    std::string name_;
    std::vector<std::string> input_names_;
    long long run_mem_;
    long long output_mem_;
    int time_cost_;
    long long peak_;
    long long impact_;
public:
    Node() : run_mem_(0), output_mem_(0), time_cost_(0), peak_(0), impact_(0) {}
    Node(std::string name, std::vector<std::string> inputs,
         long long run_mem, long long output_mem, int time_cost)
        : name_(std::move(name)), input_names_(std::move(inputs)),
          run_mem_(run_mem), output_mem_(output_mem), time_cost_(time_cost) {
        peak_ = std::max(run_mem_, output_mem_);
        impact_ = output_mem_;
    }
    const std::string& getName() const { return name_; }
    const std::vector<std::string>& getInputs() const { return input_names_; }
    long long getRunMem() const { return run_mem_; }
    long long getOutputMem() const { return output_mem_; }
    int getTimeCost() const { return time_cost_; }
    long long getPeak() const { return peak_; }
    long long getImpact() const { return impact_; }
    void setImpact(long long impact) { impact_ = impact; }
};

struct ScheduleState {
    std::vector<std::string> execution_order;
    std::vector<bool> recompute_flags; // true if this step is a recomputation of a previously executed node
    long long current_memory{0};
    long long memory_peak{0};
    long long total_time{0};
    std::unordered_set<std::string> computed;
    std::unordered_map<std::string, long long> output_memory;
};

struct Problem {
    long long total_memory{0};
    std::unordered_map<std::string, Node> nodes;
    std::unordered_map<std::string, std::unordered_set<std::string>> dependencies; // input -> consumers
    std::unordered_map<std::string, std::vector<std::string>> successors; // node -> consumers list
};
