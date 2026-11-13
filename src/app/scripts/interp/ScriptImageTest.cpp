// Laying out a .scr: an invented 32-bit DLL built here, byte by byte, with a code section, a data
// section holding its import, export and relocation tables, read at its own base and moved.
#include "app/scripts/interp/ScriptImage.h"

#include "app/scripts/interp/InventedScript.h"

#include <doctest/doctest.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

using namespace sfcop::scripts::interp;
using namespace sfcop::scripts::interp::test;

namespace {

std::uint32_t at32(const ScriptImage &image, std::uint32_t rva) {
    std::uint32_t v;
    std::memcpy(&v, image.bytes.data() + rva, 4);
    return v;
}

std::string refusal(const Pe &pe, std::uint32_t base = 0x60000000u) {
    const ScriptImageResult result = readScriptImage(pe.bytes, base);
    CHECK_FALSE(result.ok);
    return result.error;
}

} // namespace

TEST_CASE("a .scr read at its own base keeps its bytes and names its entries and imports") {
    const ScriptImageResult result = readScriptImage(invented().bytes, kLinked);
    REQUIRE_MESSAGE(result.ok, result.error);
    const ScriptImage &image = result.image;
    CHECK(image.linkedBase == 0x10000000u);
    CHECK(image.base == 0x10000000u);
    CHECK(image.size == 0x10000u);
    CHECK(image.bytes.size() == 0x10000u);
    CHECK(image.entry == 0x10001000u);
    CHECK(image.missionSpecEntry == 0x10001000u);
    CHECK(image.scriptEntry == 0x10001006u);
    CHECK(at32(image, 0x1001) == 0x10002010u);
    CHECK(image.bytes[0x1005] == 0xC3);
    CHECK(image.bytes[0] == 'M');

    REQUIRE(image.sections.size() == 2);
    CHECK(image.sections[0].rva == 0x1000u);
    CHECK(image.sections[0].size == 0x10u);
    CHECK(image.sections[0].executable);
    CHECK_FALSE(image.sections[0].writable);
    CHECK(image.sections[1].rva == 0x2000u);
    CHECK(image.sections[1].size == 0x400u);
    CHECK_FALSE(image.sections[1].executable);
    CHECK(image.sections[1].writable);

    REQUIRE(image.imports.size() == 2);
    CHECK(image.imports[0].iatRva == 0x2050u);
    CHECK(image.imports[0].name == "KERNEL32.DLL!HeapAlloc");
    CHECK(image.imports[1].iatRva == 0x2054u);
    CHECK(image.imports[1].name == "KERNEL32.DLL!#7");
}

TEST_CASE("a .scr moved to a slot has every HIGHLOW relocation and its header base moved") {
    const ScriptImageResult result = readScriptImage(invented().bytes, 0x60100000u);
    REQUIRE_MESSAGE(result.ok, result.error);
    const ScriptImage &image = result.image;
    CHECK(at32(image, 0x1001) == 0x60102010u);
    CHECK(image.pointers == std::vector<std::uint32_t>{0x1001});
    CHECK(at32(image, kOptional + 28) == 0x60100000u);
    CHECK(image.entry == 0x60101000u);
    CHECK(image.missionSpecEntry == 0x60101000u);
    CHECK(image.scriptEntry == 0x60101006u);
    // Bytes no relocation names are as in the file.
    CHECK(image.bytes[0x1000] == 0xB8);
    CHECK(at32(image, 0x2050) == 0x2060u);
}

TEST_CASE("a file that is not a mission's DLL is refused with what is wrong") {
    CHECK(refusal(invented(), 0x60001000u) == "the base 0x60001000 is not on a 64 KB boundary");

    Pe notMz = invented();
    notMz.put8(0, 'Z');
    CHECK(refusal(notMz) == "it is not a Windows program (no MZ header)");

    Pe exe = invented();
    exe.put16(0x56, 0x0102);
    CHECK(refusal(exe) == "it is not a DLL");

    Pe arm = invented();
    arm.put16(0x44, 0x01C4);
    CHECK(refusal(arm) == "it is built for machine 0x1C4, not i386");

    Pe tls = invented();
    tls.directory(9, 0x2300, 0x18);
    CHECK(refusal(tls) == "it has thread-local storage, which a mission never has");

    Pe relocType10 = invented();
    relocType10.data32(0x21C8, 0x0000A001);
    CHECK(refusal(relocType10) == "it has a relocation of type 10 at rva 0x1001");

    Pe fixed = invented();
    fixed.directory(5, 0, 0);
    CHECK(refusal(fixed) == "it has no base relocations, so it cannot be moved from 0x10000000");
    CHECK(readScriptImage(fixed.bytes, kLinked).ok);

    Pe truncated = invented();
    truncated.bytes.resize(0x500);
    CHECK(refusal(truncated) ==
          "it is truncated: 1024 bytes at file offset 0x400 are past its end");

    Pe outside = invented();
    outside.data32(0x2144, 0x20000);
    CHECK(refusal(outside) == "its export ScriptEntry lies outside the image");

    Pe nameOutside = invented();
    nameOutside.data32(0x200C, 0x10000);
    CHECK(refusal(nameOutside) == "rva 0x10000 (+1) is outside the image");
}
