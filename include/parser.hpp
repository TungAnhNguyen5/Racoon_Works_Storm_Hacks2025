#pragma once

#include "model.hpp"
#include <string>
#include <vector>
#include <istream>

struct ParsedNodeSpec {
    std::string name;
    long long run_mem{0};
    long long output_mem{0};
    int time_cost{0};
    std::vector<std::string> inputs;
};

bool parseExamplesFormat(std::istream& in, long long& total_memory,
                         std::vector<ParsedNodeSpec>& nodes_out,
                         std::string& error);

bool parseSimpleFormat(std::istream& in, long long& total_memory,
                       std::vector<ParsedNodeSpec>& nodes_out,
                       std::string& error);

Problem buildProblem(long long total_memory, const std::vector<ParsedNodeSpec>& specs);


