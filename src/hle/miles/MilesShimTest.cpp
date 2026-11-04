#include "hle/miles/ShimRows.h"

#include <doctest/doctest.h>

namespace sfcop::hle::miles {

using namespace shimrows;

TEST_CASE("every MSS32.DLL row has a shim that purges the bytes its name gives") {
    const std::string here = directoryOf(__FILE__);
    Shims shims = parseGuest(readFile(here + "/MilesGuest.cpp"));
    std::vector<Row> rows = surfaceRows(here + "/../surface.tsv", {"MSS32.DLL"});
    REQUIRE(rows.size() == 26u);
    CHECK(shims.functionOf.size() == 26u);
    for (const Row &row : rows) {
        CAPTURE(row.name);
        CHECK((row.status == "implemented" || row.status == "stub"));
        REQUIRE(shims.functionOf.count(row.name) == 1u);
        const std::string &fn = shims.functionOf[row.name];
        REQUIRE(shims.purgesOf.count(fn) == 1u);
        CHECK(shims.purgesOf[fn] == std::set<int>{row.stackBytes / 4});
    }
}

} // namespace sfcop::hle::miles
