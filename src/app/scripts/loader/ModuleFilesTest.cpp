// The library readers on hostile files: small Mach-O, ELF and PE images written byte by byte, then
// cut short, given a section offset past the end of the file, and given counts that overflow. Each
// must be refused with its reason (or read as the empty library it is), never read past the end.
//
// Every image is copied into a buffer of exactly its size before it is read, so that a run of these
// tests under AddressSanitizer (-fsanitize=address,undefined) stops at the first byte read beyond it.
#include "app/scripts/loader/NativeModuleLoader.h"

#include <doctest/doctest.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <system_error>

using namespace sfcop::scripts;

namespace {

constexpr const char *kMac = "arm64-macos-itanium";
constexpr const char *kLinux = "x86_64-linux-itanium";
constexpr const char *kWindows = "x86_64-windows-msvc";

void put(std::string &out, std::uint64_t at, std::uint64_t value, int size) {
    if (out.size() < at + static_cast<std::uint64_t>(size)) {
        out.resize(at + static_cast<std::uint64_t>(size), '\0');
    }
    for (int i = 0; i < size; ++i) {
        out[at + static_cast<std::uint64_t>(i)] = static_cast<char>(value >> (8 * i) & 0xFF);
    }
}

// Reads `image` from a buffer of exactly its size.
LibraryInfo readExact(const std::string &image, const char *hostAbi) {
    const std::unique_ptr<char[]> exact(new char[image.empty() ? 1 : image.size()]);
    std::memcpy(exact.get(), image.data(), image.size());
    return readLibrary(std::string_view(exact.get(), image.size()), hostAbi);
}

// ---- Mach-O: one LC_SYMTAB, two symbols (an exported ScriptEntry and an undefined memcpy) ----

struct MachO {
    std::uint64_t ncmds = 1, symoff = 32 + 24, nsyms = 2, stroff = 32 + 24 + 32, strsize = 29;
    std::string image() const {
        std::string b;
        put(b, 0, 0xFEEDFACF, 4);
        put(b, 4, 0x0100000C, 4); // arm64
        put(b, 12, 6, 4);         // a dynamic library
        put(b, 16, ncmds, 4);
        put(b, 20, 24, 4);
        put(b, 32, 0x2, 4); // LC_SYMTAB
        put(b, 36, 24, 4);
        put(b, 40, symoff, 4);
        put(b, 44, nsyms, 4);
        put(b, 48, stroff, 4);
        put(b, 52, strsize, 4);
        put(b, 56, 1, 4);    // _ScriptEntry
        put(b, 60, 0x0F, 1); // external, defined in a section
        put(b, 72, 14, 4);   // _memcpy
        put(b, 76, 0x01, 1); // external, undefined
        const char strings[] = "\0_ScriptEntry\0_memcpy\0\0\0\0\0\0\0\0";
        b.resize(88); // the strings sit here whatever the header says
        b.append(strings, sizeof strings - 1);
        return b;
    }
};

// ---- ELF64: a header, a section table of three entries (null, .dynsym, .dynstr) ----

struct Elf {
    // The symbols sit at 256 and the strings at 304 whatever the header says.
    std::uint64_t shoff = 64, shnum = 3, shentsize = 64, dynsymOff = 256, dynsymSize = 24 * 2,
                  dynstrOff = 304, dynstrSize = 8, link = 2;
    std::string image() const {
        std::string b;
        b.append("\x7F" "ELF", 4);
        put(b, 4, 2, 1);
        put(b, 5, 1, 1);
        put(b, 16, 3, 2);   // a shared object
        put(b, 18, 62, 2);  // x86_64
        put(b, 40, shoff, 8);
        put(b, 58, shentsize, 2);
        put(b, 60, shnum, 2);
        const std::uint64_t dynsym = 64 + 64, dynstr = 64 + 128;
        put(b, dynsym + 4, 11, 4); // SHT_DYNSYM
        put(b, dynsym + 24, dynsymOff, 8);
        put(b, dynsym + 32, dynsymSize, 8);
        put(b, dynsym + 40, link, 4);
        put(b, dynstr + 4, 3, 4); // SHT_STRTAB
        put(b, dynstr + 24, dynstrOff, 8);
        put(b, dynstr + 32, dynstrSize, 8);
        put(b, 256 + 24, 1, 4); // the second symbol: global, undefined, named at 1
        put(b, 256 + 24 + 4, 0x10, 1);
        b.resize(304);
        b.append("\0memcpy\0", 8);
        return b;
    }
};

// ---- PE32+: DOS header, PE header at 0x40, optional header of 240 bytes, one section ----

struct Pe {
    std::uint64_t peAt = 0x40, nsec = 1, optsize = 240, directories = 16, exportRva = 0, importRva = 0;
    std::string image() const {
        std::string b;
        b.append("MZ", 2);
        put(b, 0x3C, peAt, 4);
        put(b, peAt, 0x00004550, 4);
        put(b, peAt + 4, 0x8664, 2);
        put(b, peAt + 6, nsec, 2);
        put(b, peAt + 20, optsize, 2);
        put(b, peAt + 22, 0x2022, 2); // a DLL
        const std::uint64_t opt = peAt + 24;
        put(b, opt, 0x20B, 2);
        put(b, opt + 108, directories, 4);
        put(b, opt + 112, exportRva, 4);
        put(b, opt + 120, importRva, 4);
        b.resize(opt + optsize + 40 * 1);
        put(b, opt + optsize + 12, 0x1000, 4); // the section: rva, raw size, raw offset
        put(b, opt + optsize + 16, 0x200, 4);
        put(b, opt + optsize + 20, 0x200, 4);
        b.resize(0x400);
        return b;
    }
};

} // namespace

TEST_CASE("hand-written images read as the libraries they are") {
    const LibraryInfo mac = readExact(MachO{}.image(), kMac);
    CHECK(mac.notLoadable == "");
    CHECK(mac.exportedSymbols == std::vector<std::string>{"ScriptEntry"});
    CHECK(mac.undefinedSymbols == std::vector<std::string>{"memcpy"});
    const LibraryInfo elf = readExact(Elf{}.image(), kLinux);
    CHECK(elf.notLoadable == "");
    CHECK(elf.undefinedSymbols == std::vector<std::string>{"memcpy"});
    CHECK(readExact(Pe{}.image(), kWindows).notLoadable == "");
}

TEST_CASE("a truncated header is refused at every length, without reading past the end") {
    struct Case {
        std::string image;
        const char *host;
    };
    for (const Case &c : {Case{MachO{}.image(), kMac}, Case{Elf{}.image(), kLinux}, Case{Pe{}.image(), kWindows}}) {
        CAPTURE(c.host);
        std::size_t refused = 0;
        for (std::size_t length = 0; length < c.image.size(); ++length) {
            const LibraryInfo info = readExact(c.image.substr(0, length), c.host);
            refused += info.notLoadable.empty() ? 0 : 1;
        }
        CHECK(refused > 0);
    }
    // Exactly where each format's own header check ends.
    CHECK(readExact(MachO{}.image().substr(0, 31), kMac).notLoadable == "not a 64-bit Mach-O file");
    CHECK(readExact(MachO{}.image().substr(0, 50), kMac).notLoadable == "truncated load commands");
    CHECK(readExact(MachO{}.image().substr(0, 100), kMac).notLoadable ==
          "the symbol table runs past the end of the file");
    CHECK(readExact(Elf{}.image().substr(0, 63), kLinux).notLoadable == "not a little-endian 64-bit ELF file");
    CHECK(readExact(Elf{}.image().substr(0, 100), kLinux).notLoadable ==
          "the section table runs past the end of the file");
    CHECK(readExact(Pe{}.image().substr(0, 0x40 + 24 + 112), kWindows).notLoadable ==
          "the optional header runs past the end of the file");
    CHECK(readExact(Pe{}.image().substr(0, 0x3C), kWindows).notLoadable == "not a PE file");
}

TEST_CASE("a section or table offset past the end of the file is refused with its reason") {
    MachO symbols;
    symbols.symoff = 0xFFFFFFF0;
    CHECK(readExact(symbols.image(), kMac).notLoadable == "the symbol table runs past the end of the file");
    MachO strings;
    strings.stroff = 0xFFFFFFF0;
    CHECK(readExact(strings.image(), kMac).notLoadable == "the symbol table runs past the end of the file");

    Elf table;
    table.shoff = 0xFFFFFFFFFFFFFF00ull;
    CHECK(readExact(table.image(), kLinux).notLoadable == "the section table runs past the end of the file");
    Elf dynsym;
    dynsym.dynsymOff = 0xFFFFFFFFFFFFFF00ull;
    CHECK(readExact(dynsym.image(), kLinux).notLoadable ==
          "the dynamic symbol table runs past the end of the file");
    Elf link;
    link.link = 99;
    CHECK(readExact(link.image(), kLinux).notLoadable == "the dynamic symbol table runs past the end of the file");

    std::string header = Pe{}.image();
    put(header, 0x3C, 0xFFFFFFF0, 4); // the PE header offset
    CHECK(readExact(header, kWindows).notLoadable == "not a PE file");
    // A directory that points past the file, or into no section, reads as no exports and no imports.
    Pe directories;
    directories.exportRva = 0x7FFFFFFF;
    directories.importRva = 0x7FFFFFF0;
    const LibraryInfo info = readExact(directories.image(), kWindows);
    CHECK(info.notLoadable == "");
    CHECK(info.exportedSymbols.empty());
    CHECK(info.undefinedSymbols.empty());
}

TEST_CASE("counts that overflow are refused or read as nothing, never trusted") {
    MachO commands;
    commands.ncmds = 0xFFFFFFFF;
    CHECK(readExact(commands.image(), kMac).notLoadable == "truncated load commands");
    MachO symbols;
    symbols.nsyms = 0xFFFFFFFF;
    CHECK(readExact(symbols.image(), kMac).notLoadable == "the symbol table runs past the end of the file");
    MachO strings;
    strings.strsize = 0xFFFFFFFF;
    CHECK(readExact(strings.image(), kMac).notLoadable == "the symbol table runs past the end of the file");
    // A universal file that declares four billion architectures and holds none.
    const std::string fat("\xCA\xFE\xBA\xBE\xFF\xFF\xFF\xFF", 8);
    CHECK(readExact(fat, kMac).notLoadable == "a universal file with no arm64 part");

    Elf sections;
    sections.shnum = 0xFFFF;
    CHECK(readExact(sections.image(), kLinux).notLoadable == "the section table runs past the end of the file");
    Elf size;
    size.dynsymSize = 0xFFFFFFFFFFFFFFFFull;
    CHECK(readExact(size.image(), kLinux).notLoadable == "the dynamic symbol table runs past the end of the file");
    Elf entry;
    entry.shentsize = 0xFFFF;
    CHECK(readExact(entry.image(), kLinux).notLoadable == "the section table runs past the end of the file");

    Pe sectionsPe;
    sectionsPe.nsec = 0xFFFF;
    sectionsPe.exportRva = 0x1000;
    sectionsPe.importRva = 0x1100;
    CHECK(readExact(sectionsPe.image(), kWindows).notLoadable == "");
    Pe optional;
    optional.optsize = 0xFFFF;
    CHECK(readExact(optional.image(), kWindows).notLoadable == "the optional header runs past the end of the file");
    Pe directories;
    directories.directories = 0xFFFFFFFF;
    directories.exportRva = 0x1000;
    CHECK(readExact(directories.image(), kWindows).notLoadable == "");
}

// ---- the same images read from their files ----

namespace {

// A folder of its own for one test, removed with everything in it at the end.
struct ScratchFolder {
    std::filesystem::path path;
    ScratchFolder() {
        static int count = 0;
        path = std::filesystem::temp_directory_path() /
               ("sfcop-modulefiles-" + std::to_string(reinterpret_cast<std::uintptr_t>(this)) + "-" +
                std::to_string(++count));
        std::filesystem::create_directories(path);
    }
    ~ScratchFolder() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
    std::filesystem::path write(const std::string &name, const std::string &bytes) const {
        const std::filesystem::path file = path / name;
        std::ofstream out(file, std::ios::binary);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        return file;
    }
};

void checkSame(const LibraryInfo &fromFile, const LibraryInfo &fromMemory) {
    CHECK(fromFile.notLoadable == fromMemory.notLoadable);
    CHECK(fromFile.undefinedSymbols == fromMemory.undefinedSymbols);
    CHECK(fromFile.exportedSymbols == fromMemory.exportedSymbols);
    CHECK(fromFile.abiTag.has_value() == fromMemory.abiTag.has_value());
}

} // namespace

TEST_CASE("a library read from its file answers what its bytes in memory answer, at every length") {
    const ScratchFolder folder;
    struct Case {
        std::string image;
        const char *host;
    };
    for (const Case &c : {Case{MachO{}.image(), kMac}, Case{Elf{}.image(), kLinux}, Case{Pe{}.image(), kWindows}}) {
        CAPTURE(c.host);
        for (std::size_t length = 0; length <= c.image.size(); ++length) {
            CAPTURE(length);
            const std::string cut = c.image.substr(0, length);
            checkSame(readLibraryFile(folder.write("lib.bin", cut), c.host), readExact(cut, c.host));
        }
    }
    const LibraryInfo mac = readLibraryFile(folder.write("mac.dylib", MachO{}.image()), kMac);
    CHECK(mac.exportedSymbols == std::vector<std::string>{"ScriptEntry"});
    CHECK(mac.undefinedSymbols == std::vector<std::string>{"memcpy"});
}

TEST_CASE("a library read from its file reads its headers and symbols, not its code") {
    const ScratchFolder folder;
    // 8 MB of code the readers never look at, after the symbols (as a module's __TEXT would sit).
    std::string withCode = MachO{}.image();
    withCode.resize(8u * 1024u * 1024u, '\xCC');
    std::uint64_t read = 0;
    const LibraryInfo first = readLibraryFile(folder.write("first.dylib", withCode), kMac, &read);
    CHECK(first.exportedSymbols == std::vector<std::string>{"ScriptEntry"});
    CHECK(first.undefinedSymbols == std::vector<std::string>{"memcpy"});
    CHECK(read == 16384); // the one block holding the header, the table and its strings

    // The table and its strings after the code, as a module's __LINKEDIT sits: two blocks.
    MachO late;
    late.symoff = 8u * 1024u * 1024u;
    late.stroff = late.symoff + 32;
    std::string lateImage = late.image(); // header and load commands only, up to its strings
    lateImage.resize(static_cast<std::size_t>(late.symoff), '\xCC');
    std::string table = MachO{}.image().substr(56, 32 + 29); // the two entries and the strings
    lateImage += table;
    const LibraryInfo last = readLibraryFile(folder.write("last.dylib", lateImage), kMac, &read);
    CHECK(last.notLoadable == "");
    CHECK(last.exportedSymbols == std::vector<std::string>{"ScriptEntry"});
    CHECK(last.undefinedSymbols == std::vector<std::string>{"memcpy"});
    CHECK(read == 16384 + (lateImage.size() - late.symoff)); // the first block and the last, short one
}

TEST_CASE("a library file that cannot be opened is not loadable") {
    const ScratchFolder folder;
    std::uint64_t read = 7;
    CHECK(readLibraryFile(folder.path / "absent.dylib", kMac, &read).notLoadable == "cannot be read");
    CHECK(read == 0);
}
