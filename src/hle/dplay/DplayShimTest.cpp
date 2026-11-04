#include "hle/miles/ShimRows.h"

#include <doctest/doctest.h>

#include <regex>

namespace sfcop::hle::dplay {

using namespace shimrows;

namespace {

// The method names of one interface's table, in the order DplayGuest.cpp lists them.
std::vector<std::string> tableOrder(const std::string &text, const std::string &interface) {
    std::vector<std::string> names;
    const std::regex entry("X\\(\"" + interface + "::(\\w+)\"");
    for (auto it = std::sregex_iterator(text.begin(), text.end(), entry);
         it != std::sregex_iterator(); ++it) {
        names.push_back((*it)[1]);
    }
    return names;
}

} // namespace

TEST_CASE("every DirectPlay row has a shim that purges the bytes the row gives") {
    const std::string here = directoryOf(__FILE__);
    const std::string text = readFile(here + "/DplayGuest.cpp");
    Shims shims = parseGuest(text);
    std::vector<Row> rows =
        surfaceRows(here + "/../surface.tsv", {"IDirectPlay4A", "IDirectPlayLobby3A"});
    REQUIRE(rows.size() == 53u + 19u);
    std::size_t used = 0;
    std::vector<std::string> play, lobby;
    for (const Row &row : rows) {
        std::string name = row.module + "::" + row.name;
        CAPTURE(name);
        CHECK((row.status == "implemented" || row.status == "stub" || row.status == "unused"));
        used += row.status != "unused";
        REQUIRE(shims.functionOf.count(name) == 1u);
        const std::string &fn = shims.functionOf[name];
        REQUIRE(shims.purgesOf.count(fn) == 1u);
        CHECK(shims.purgesOf[fn] == std::set<int>{row.stackBytes / 4});
        (row.module == "IDirectPlay4A" ? play : lobby).push_back(row.name);
    }
    // The 28 + 7 methods the game calls.
    CHECK(used == 35u);
    // Each vtable is in slot order.
    CHECK(tableOrder(text, "IDirectPlay4A") == play);
    CHECK(tableOrder(text, "IDirectPlayLobby3A") == lobby);
}

} // namespace sfcop::hle::dplay
