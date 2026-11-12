// An invented 32-bit DLL in the form of a .scr, built byte by byte for the interpreter's tests: a
// code section and a data section holding its import, export and relocation tables. Test-only.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

namespace sfcop::scripts::interp::test {

inline constexpr std::uint32_t kLinked = 0x10000000u;
inline constexpr std::uint32_t kOptional = 0x58; // 0x40 PE signature + 4 + the 20-byte file header
inline constexpr std::uint32_t kSections = kOptional + 0xE0;

class Pe {
public:
    std::vector<std::uint8_t> bytes = std::vector<std::uint8_t>(0x800, 0);

    void put8(std::uint32_t at, std::uint8_t v) { bytes.at(at) = v; }
    void put16(std::uint32_t at, std::uint16_t v) {
        put8(at, static_cast<std::uint8_t>(v));
        put8(at + 1, static_cast<std::uint8_t>(v >> 8));
    }
    void put32(std::uint32_t at, std::uint32_t v) {
        put16(at, static_cast<std::uint16_t>(v));
        put16(at + 2, static_cast<std::uint16_t>(v >> 16));
    }
    void text(std::uint32_t at, const char *s) { std::memcpy(bytes.data() + at, s, std::strlen(s) + 1); }
    // .data is at rva 0x2000, file offset 0x400.
    void data32(std::uint32_t rva, std::uint32_t v) { put32(rva - 0x2000 + 0x400, v); }
    void dataText(std::uint32_t rva, const char *s) { text(rva - 0x2000 + 0x400, s); }
    void directory(unsigned index, std::uint32_t rva, std::uint32_t size) {
        put32(kOptional + 96 + 8 * index, rva);
        put32(kOptional + 100 + 8 * index, size);
    }
};

// What goes in the DLL's .text (at rva 0x1000, at most 0x200 bytes): its code, the rvas of its
// entry point and two exports, and the rvas its HIGHLOW relocations patch.
struct InventedCode {
    std::vector<std::uint8_t> code{0xB8, 0x10, 0x20, 0x00, 0x10, 0xC3, 0xC3};
    std::uint32_t entry = 0x1000;
    std::uint32_t missionSpecEntry = 0x1000;
    std::uint32_t scriptEntry = 0x1006;
    std::vector<std::uint32_t> relocations{0x1001};
};

// The DLL: .text at rva 0x1000 holds the code (by default `mov eax, 0x10002010 ; ret`, the
// immediate relocated, and `ret` at 0x1006); .data at rva 0x2000 (0x400 bytes, writable) holds
// the tables: the import of kernel32.dll!HeapAlloc with its address slot at 0x2050, the exports
// at 0x2100 and the relocations at 0x21C0.
inline Pe invented(const InventedCode &c = {}) {
    Pe pe;
    pe.put8(0, 'M');
    pe.put8(1, 'Z');
    pe.put32(0x3C, 0x40);
    pe.text(0x40, "PE");
    pe.put16(0x44, 0x014C); // i386
    pe.put16(0x46, 2);      // two sections
    pe.put16(0x54, 0xE0);   // optional header size
    pe.put16(0x56, 0x2102); // DLL, 32-bit, executable
    pe.put16(kOptional, 0x010B);
    pe.put32(kOptional + 16, c.entry);
    pe.put32(kOptional + 28, kLinked);
    pe.put32(kOptional + 32, 0x1000);
    pe.put32(kOptional + 36, 0x200);
    pe.put32(kOptional + 56, 0x3000); // image size
    pe.put32(kOptional + 60, 0x200);  // headers size
    pe.put32(kOptional + 92, 16);
    pe.directory(0, 0x2100, 0x40);
    pe.directory(1, 0x2000, 0x28);
    const std::uint32_t relocationSize = 8 + 2 * static_cast<std::uint32_t>((c.relocations.size() + 1) / 2 * 2);
    pe.directory(5, 0x21C0, relocationSize);

    pe.text(kSections, ".text");
    pe.put32(kSections + 8, std::max<std::uint32_t>(0x10, static_cast<std::uint32_t>(c.code.size())));
    pe.put32(kSections + 12, 0x1000);
    pe.put32(kSections + 16, 0x200);
    pe.put32(kSections + 20, 0x200);
    pe.put32(kSections + 36, 0x60000020);
    pe.text(kSections + 40, ".data");
    pe.put32(kSections + 48, 0x400);
    pe.put32(kSections + 52, 0x2000);
    pe.put32(kSections + 56, 0x400);
    pe.put32(kSections + 60, 0x400);
    pe.put32(kSections + 76, 0xC0000040);

    std::memcpy(pe.bytes.data() + 0x200, c.code.data(), c.code.size());

    // Imports: one descriptor, its lookup table at 0x2040, its address table at 0x2050.
    pe.data32(0x2000, 0x2040);
    pe.data32(0x200C, 0x2070);
    pe.data32(0x2010, 0x2050);
    for (std::uint32_t table : {0x2040u, 0x2050u}) {
        pe.data32(table, 0x2060);
        pe.data32(table + 4, 0x80000007u);
    }
    pe.dataText(0x2062, "HeapAlloc");
    pe.dataText(0x2070, "kernel32.dll");

    // Exports: two functions, two names.
    pe.data32(0x2100 + 20, 2);
    pe.data32(0x2100 + 24, 2);
    pe.data32(0x2100 + 28, 0x2140);
    pe.data32(0x2100 + 32, 0x2150);
    pe.data32(0x2100 + 36, 0x2160);
    pe.data32(0x2140, c.missionSpecEntry);
    pe.data32(0x2144, c.scriptEntry);
    pe.data32(0x2150, 0x2170);
    pe.data32(0x2154, 0x2190);
    pe.data32(0x2160, 0x00010000); // ordinals 0, 1
    pe.dataText(0x2170, "MissionSpecEntry");
    pe.dataText(0x2190, "ScriptEntry");

    // Relocations: one block for page 0x1000, the HIGHLOWs, an ABSOLUTE pad to a 4-byte size.
    pe.data32(0x21C0, 0x1000);
    pe.data32(0x21C4, relocationSize);
    for (std::size_t i = 0; i < c.relocations.size(); ++i) {
        pe.put16(static_cast<std::uint32_t>(0x21C8 - 0x2000 + 0x400 + 2 * i),
                 static_cast<std::uint16_t>(0x3000 | (c.relocations[i] - 0x1000)));
    }
    return pe;
}

} // namespace sfcop::scripts::interp::test
