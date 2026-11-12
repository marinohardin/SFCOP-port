// A mission script's image as the interpreter maps it (DESIGN §5.9): a compiled .scr, a 32-bit
// Windows DLL, read from the player's own file, with its sections laid out at a guest base and
// every base relocation applied for that base. Nothing here runs or trusts the file: every offset
// is checked against it before it is read, and a file that breaks the format is refused with a
// sentence that says what is wrong.
//
// What a .scr may hold is narrower than what a DLL may: i386, a DLL, no TLS, no forwarded export,
// HIGHLOW relocations only, imports by name (an ordinal import cannot be on the allow-list).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace sfcop::scripts::interp {

struct ScriptSection {
    std::uint32_t rva = 0;
    std::uint32_t size = 0; // in the image: the virtual size, or the raw size when that is 0
    bool executable = false;
    bool writable = false;
};

// A Windows import: the slot at base + iatRva gets the address of `name`, written DLL!Name as on
// the allow-list (KERNEL32.DLL!HeapAlloc; an ordinal is DLL!#n).
struct ScriptImport {
    std::uint32_t iatRva = 0;
    std::string name;
};

struct ScriptImage {
    std::uint32_t linkedBase = 0; // the base the file was linked at
    std::uint32_t base = 0;       // the base it is laid out at here
    std::uint32_t size = 0;       // the image's size from base, rounded up to 64 KB
    std::uint32_t entry = 0;      // guest address of the DLL entry point, 0 when none
    std::uint32_t missionSpecEntry = 0; // guest addresses of the contract's two exports, 0 when
    std::uint32_t scriptEntry = 0;      // the file does not export them
    std::vector<std::uint8_t> bytes;    // `size` bytes: headers and sections, relocated
    std::vector<ScriptSection> sections;
    std::vector<ScriptImport> imports; // in the import table's own order
    std::vector<std::uint32_t> pointers; // the rvas the base relocations fix up, in table order
};

struct ScriptImageResult {
    bool ok = false;
    ScriptImage image;
    std::string error; // when not ok: what is wrong with the file, one sentence
};

// Reads `file` and lays it out at `base` (a multiple of 64 KB).
ScriptImageResult readScriptImage(const std::vector<std::uint8_t> &file, std::uint32_t base);

} // namespace sfcop::scripts::interp
