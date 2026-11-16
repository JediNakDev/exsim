// Layering checks over the include graph. Run from the repository root,
// like the fixture tests.
// * The exchange core depends on the engine, not on the market data layer:
//   no header reachable from server/exchange_core.hpp or server/core/
//   includes an md/ header.
// * The management service (api/) never reaches the server runtime; the
//   daemon's runtime snapshot lives with the control socket in ipc/.

#include "test/framework/test.hpp"

#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

namespace {

void collect_project_includes(const std::string& header, std::set<std::string>& seen);

// Adds the project headers ("dir/name.hpp" or <dir/name.hpp> found under
// lib/include) that the file at `path` includes, and everything they reach.
void collect_includes_of_file(const std::filesystem::path& path, std::set<std::string>& seen) {
    std::ifstream file(path);
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

// Project headers reachable from `header`, itself included.
void collect_project_includes(const std::string& header, std::set<std::string>& seen) {
    if (!seen.insert(header).second) return;
    collect_includes_of_file("lib/include/" + header, seen);
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

TEST_CASE(server_core_layering, management_api_does_not_reach_the_server_runtime) {
    std::set<std::string> reachable;
    std::size_t files = 0;
    for (const char* directory : {"lib/include/api", "lib/src/api"}) {
        for (const auto& entry : std::filesystem::directory_iterator(directory)) {
            collect_includes_of_file(entry.path(), reachable);
            ++files;
        }
    }
    REQUIRE(files > 1);
    REQUIRE(reachable.contains("api/json.hpp"));
    for (const std::string& header : reachable) {
        if (header.starts_with("server/runtime/") || header == "server/server_runtime.hpp" ||
            header == "server/exchange_core.hpp")
            FAIL("api/ reaches " + header);
    }
}
