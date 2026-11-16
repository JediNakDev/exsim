// The exchange core depends on the engine, not on the market data layer:
// no header reachable from server/exchange_core.hpp or server/core/ includes
// an md/ header. Run from the repository root, like the fixture tests.

#include "test/framework/test.hpp"

#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

namespace {

// Project headers ("dir/name.hpp" or <dir/name.hpp> found under lib/include)
// reachable from `header`, itself included.
void collect_project_includes(const std::string& header, std::set<std::string>& seen) {
    if (!seen.insert(header).second) return;
    std::ifstream file("lib/include/" + header);
    std::string line;
    while (std::getline(file, line)) {
        if (!line.starts_with("#include ")) continue;
        const std::size_t open = line.find_first_of("\"<");
        const std::size_t close = line.find_first_of("\">", open + 1);
        if (open == std::string::npos || close == std::string::npos) continue;
        const std::string included = line.substr(open + 1, close - open - 1);
        if (std::filesystem::exists("lib/include/" + included)) collect_project_includes(included, seen);
    }
}

}  // namespace

TEST_CASE(server_core_layering, core_headers_do_not_reach_market_data_headers) {
    std::vector<std::string> roots{"server/exchange_core.hpp"};
    for (const auto& entry : std::filesystem::directory_iterator("lib/include/server/core")) {
        roots.push_back("server/core/" + entry.path().filename().string());
    }
    REQUIRE(roots.size() > 1);
    for (const std::string& root : roots) {
        std::set<std::string> reachable;
        collect_project_includes(root, reachable);
        if (root == "server/exchange_core.hpp") REQUIRE(reachable.contains("server/core/policies.hpp"));
        for (const std::string& header : reachable) {
            if (header.starts_with("md/")) FAIL(root + " reaches " + header);
        }
    }
}
