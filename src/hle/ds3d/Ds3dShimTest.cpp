#include "hle/miles/ShimRows.h"

#include <doctest/doctest.h>

namespace sfcop::hle::ds3d {

using namespace shimrows;

TEST_CASE("every DirectSound row has a shim that purges the bytes the row gives") {
    const std::string here = directoryOf(__FILE__);
    Shims shims = parseGuest(readFile(here + "/Ds3dGuest.cpp"));
    std::vector<Row> rows =
        surfaceRows(here + "/../surface.tsv", {"DSOUND.DLL", "IDirectSound", "IDirectSoundBuffer",
                                               "IDirectSound3DBuffer", "IDirectSound3DListener"});
    // DirectSoundCreate, then 11 + 21 + 21 + 18 methods.
    REQUIRE(rows.size() == 72u);
    for (const Row &row : rows) {
        // An import is answered by its name, a COM method by Interface::Method.
        std::string name = row.module == "DSOUND.DLL" ? row.name : row.module + "::" + row.name;
        CAPTURE(name);
        CHECK((row.status == "implemented" || row.status == "stub"));
        REQUIRE(shims.functionOf.count(name) == 1u);
        const std::string &fn = shims.functionOf[name];
        REQUIRE(shims.purgesOf.count(fn) == 1u);
        CHECK(shims.purgesOf[fn] == std::set<int>{row.stackBytes / 4});
    }
    // DirectSoundCreate is imported by ordinal 1 as well.
    CHECK(shims.functionOf["dsound.dll#1"] == shims.functionOf["DirectSoundCreate"]);
}

} // namespace sfcop::hle::ds3d
