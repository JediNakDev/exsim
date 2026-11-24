// Layering checks over the include graph. Run from the repository root,
// like the fixture tests.
// * The exchange core depends on the engine, not on the market data layer:
//   no header reachable from server/core/ (exchange_core.hpp included)
//   includes an md/ header.

#include "test/framework/test.hpp"

#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

namespace {

void collect_project_includes(const std::string& header, std::set<std::string>& seen);

// Adds the project headers ("dir/name.hpp" or <dir/name.hpp> found under
// src) that the file at `path` includes, and everything they reach.
void collect_includes_of_file(const std::filesystem::path& path, std::set<std::string>& seen) {
    std::ifstream file(path);
    std::string line;
    while (std::getline(file, line)) {
        if (!line.starts_with("#include ")) continue;
        const std::size_t open = line.find_first_of("\"<");
        const std::size_t close = line.find_first_of("\">", open + 1);
        if (open == std::string::npos || close == std::string::npos) continue;
        const std::string included = line.substr(open + 1, close - open - 1);
        if (std::filesystem::exists("src/" + included)) collect_project_includes(included, seen);
    }
}

// Project headers reachable from `header`, itself included.
void collect_project_includes(const std::string& header, std::set<std::string>& seen) {
    if (!seen.insert(header).second) return;
    collect_includes_of_file("src/" + header, seen);
}

}  // namespace

TEST_CASE(server_core_layering, core_headers_do_not_reach_market_data_headers) {
    std::vector<std::string> roots;
    for (const auto& entry : std::filesystem::directory_iterator("src/server/core")) {
        roots.push_back("server/core/" + entry.path().filename().string());
    }
    REQUIRE(roots.size() > 1);
    for (const std::string& root : roots) {
        std::set<std::string> reachable;
        collect_project_includes(root, reachable);
        if (root == "server/core/exchange_core.hpp") REQUIRE(reachable.contains("server/core/policies.hpp"));
        for (const std::string& header : reachable) {
            if (header.starts_with("md/")) FAIL(root + " reaches " + header);
        }
    }
}
