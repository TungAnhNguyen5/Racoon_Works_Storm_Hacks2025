#include "parser.hpp"
#include <sstream>
#include <unordered_set>

static inline std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

static inline std::vector<std::string> splitCommaList(const std::string& value) {
    std::vector<std::string> out;
    std::string token;
    std::stringstream ss(value);
    while (std::getline(ss, token, ',')) {
        token = trim(token);
        if (!token.empty() && token != "-") out.push_back(token);
    }
    return out;
}

bool parseSimpleFormat(std::istream& in, long long& total_memory,
                       std::vector<ParsedNodeSpec>& nodes_out,
                       std::string& error) {
    total_memory = -1;
    nodes_out.clear();
    std::string line;
    size_t line_no = 0;
    while (std::getline(in, line)) {
        ++line_no;
        std::string raw = trim(line);
        if (raw.empty() || raw[0] == '#') continue;
        if (raw.rfind("total_memory:", 0) == 0) {
            std::string val = trim(raw.substr(std::string("total_memory:").size()));
            try { total_memory = std::stoll(val); } catch (...) {
                error = "Invalid total_memory on line " + std::to_string(line_no);
                return false;
            }
            continue;
        }
        if (raw.rfind("node ", 0) == 0) {
            std::stringstream ss(raw);
            std::string kw; ss >> kw; // node
            ParsedNodeSpec spec{};
            if (!(ss >> spec.name >> spec.run_mem >> spec.output_mem >> spec.time_cost)) {
                error = "Invalid node header on line " + std::to_string(line_no);
                return false;
            }
            std::string rest; std::getline(ss, rest); rest = trim(rest);
            if (!rest.empty()) {
                auto pos = rest.find("inputs=");
                if (pos == std::string::npos) { error = "Missing inputs= on line " + std::to_string(line_no); return false; }
                std::string inputs_str = trim(rest.substr(pos + 7));
                spec.inputs = splitCommaList(inputs_str);
            }
            nodes_out.push_back(std::move(spec));
            continue;
        }
    }
    if (total_memory < 0) { error = "total_memory not specified"; return false; }
    if (nodes_out.empty()) { error = "No nodes specified"; return false; }
    return true;
}

// Example format: first line is "Return <total_memory>"
// Each subsequent line: "<id> <name> <num_inputs> [input ids...] <run_mem> <time_cost>"
// Inputs are listed as numeric ids referring to earlier nodes; names appear like "ExpandDims-op0"
bool parseExamplesFormat(std::istream& in, long long& total_memory,
                         std::vector<ParsedNodeSpec>& nodes_out,
                         std::string& error) {
    total_memory = -1;
    nodes_out.clear();
    std::string header;
    if (!std::getline(in, header)) { error = "Empty file"; return false; }
    {
        std::stringstream hs(header);
        std::string ret;
        if (!(hs >> ret >> total_memory) || ret != "Return") {
            error = "Expected 'Return <total_memory>' header";
            return false;
        }
    }

    // We'll parse once to capture fields, then a second pass to resolve ids to names
    struct Row {
        int id{-1};
        std::string name;
        int num_inputs{0};
        std::vector<int> input_ids;
        long long workspace_mem{0};
        long long output_mem{0};
        long time{0};
    };
    std::vector<Row> rows;
    rows.reserve(1024);

    std::string line;
    while (std::getline(in, line)) {
        std::string raw = trim(line);
        if (raw.empty()) continue;
        std::stringstream ss(raw);
        Row r; r.input_ids.clear();
        if (!(ss >> r.id >> r.name >> r.num_inputs)) continue;
        for (int i = 0; i < r.num_inputs; ++i) {
            int iid = -1; if (!(ss >> iid)) { iid = -1; } r.input_ids.push_back(iid);
        }
        long long ws = 0, outm = 0;
        long t = 0;
        ss >> ws; ss >> outm; ss >> t;
        r.workspace_mem = std::max<long long>(ws, 0);
        r.output_mem = std::max<long long>(outm, 0);
        r.time = std::max<long>(t, 0);
        rows.push_back(std::move(r));
    }

    // Build id->unique_name map for dependency resolution
    std::unordered_map<int, std::string> id_to_name;
    id_to_name.reserve(rows.size());
    for (const auto& r : rows) {
        id_to_name[r.id] = r.name + "_id" + std::to_string(r.id);
    }

    // Emit ParsedNodeSpec using unique names for nodes but original names preserved
    nodes_out.reserve(rows.size());
    for (const auto& r : rows) {
        ParsedNodeSpec spec{};
        spec.name = r.name + "_id" + std::to_string(r.id);  // Use unique name for this node
        for (int iid : r.input_ids) {
            auto it = id_to_name.find(iid);
            if (it != id_to_name.end()) spec.inputs.push_back(it->second);
        }
        spec.run_mem = r.workspace_mem;
        spec.output_mem = r.output_mem;
        spec.time_cost = static_cast<int>(r.time);
        nodes_out.push_back(std::move(spec));
    }

    if (nodes_out.empty()) { error = "No nodes parsed"; return false; }
    return true;
}

Problem buildProblem(long long total_memory, const std::vector<ParsedNodeSpec>& specs) {
    Problem prob; prob.total_memory = total_memory;
    for (const auto& s : specs) {
        prob.nodes.emplace(s.name, Node(s.name, s.inputs, s.run_mem, s.output_mem, s.time_cost));
    }
    for (const auto& s : specs) {
        for (const auto& input : s.inputs) {
            prob.dependencies[input].insert(s.name);
            prob.successors[input].push_back(s.name);
        }
        prob.successors[s.name];
    }
    return prob;
}
