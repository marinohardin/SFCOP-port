// Reading a module from its files (ModuleContract.md §3, §4): module.json, and the library's
// version tag, undefined symbols and exports, without mapping it. Mach-O (64-bit, thin or
// universal), ELF64 and PE32+ are read on every host; a file of another host's format or
// architecture is not loadable here.
#include "app/scripts/loader/NativeModuleLoader.h"

#include <cstring>
#include <fstream>
#include <iterator>
#include <map>

namespace sfcop::scripts {

namespace {

// ---- bytes ----

class Bytes {
public:
    explicit Bytes(std::string_view data) : data_(data) {}
    bool has(std::uint64_t offset, std::uint64_t size) const {
        return offset <= data_.size() && size <= data_.size() - offset;
    }
    std::uint64_t le(std::uint64_t offset, int size) const {
        std::uint64_t v = 0;
        for (int i = size - 1; i >= 0; --i) {
            v = v << 8 | static_cast<unsigned char>(data_[offset + static_cast<std::uint64_t>(i)]);
        }
        return v;
    }
    std::uint64_t be(std::uint64_t offset, int size) const {
        std::uint64_t v = 0;
        for (int i = 0; i < size; ++i) {
            v = v << 8 | static_cast<unsigned char>(data_[offset + static_cast<std::uint64_t>(i)]);
        }
        return v;
    }
    // The NUL-terminated string at `offset`, bounded by `end`; empty when it runs past.
    std::string cstring(std::uint64_t offset, std::uint64_t end) const {
        if (end > data_.size()) {
            end = data_.size();
        }
        std::string out;
        for (std::uint64_t i = offset; i < end; ++i) {
            if (data_[i] == '\0') {
                return out;
            }
            out += data_[i];
        }
        return {};
    }
    std::string_view slice(std::uint64_t offset, std::uint64_t size) const {
        return data_.substr(offset, size);
    }
    std::size_t size() const { return data_.size(); }

private:
    std::string_view data_;
};

std::optional<ModuleAbiTag> tagFrom(const Bytes &b, std::uint64_t offset) {
    if (!b.has(offset, sizeof(ModuleAbiTag))) {
        return std::nullopt;
    }
    ModuleAbiTag tag{};
    std::memcpy(&tag, b.slice(offset, sizeof tag).data(), sizeof tag);
    return tag;
}

std::string hostArch(std::string_view hostAbi) {
    return std::string(hostAbi.substr(0, hostAbi.find('-')));
}

bool hostIs(std::string_view hostAbi, std::string_view system) {
    return hostAbi.find(system) != std::string_view::npos;
}

LibraryInfo notLoadable(std::string why) {
    LibraryInfo info;
    info.notLoadable = std::move(why);
    return info;
}

// ---- Mach-O ----

constexpr std::uint32_t kMhMagic64 = 0xFEEDFACFu;
constexpr std::uint32_t kFatMagic = 0xCAFEBABEu;
constexpr std::uint32_t kCpuArm64 = 0x0100000Cu;
constexpr std::uint32_t kCpuX86_64 = 0x01000007u;
constexpr std::uint32_t kMhDylib = 6;
constexpr std::uint32_t kLcSymtab = 0x2;
constexpr std::uint32_t kLcDysymtab = 0xB;
constexpr std::uint32_t kLcSegment64 = 0x19;

std::string cpuName(std::uint64_t cpu) {
    return cpu == kCpuArm64 ? "arm64" : cpu == kCpuX86_64 ? "x86_64" : "an unknown architecture";
}

std::string stripUnderscore(std::string name) {
    if (!name.empty() && name[0] == '_') {
        name.erase(0, 1);
    }
    return name;
}

LibraryInfo readMachO(const Bytes &b, std::uint64_t base, std::uint64_t size, std::uint64_t wantCpu) {
    if (!b.has(base, 32) || b.le(base, 4) != kMhMagic64) {
        return notLoadable("not a 64-bit Mach-O file");
    }
    const std::uint64_t cpu = b.le(base + 4, 4);
    if (cpu != wantCpu) {
        return notLoadable("built for " + cpuName(cpu) + ", this host is " + cpuName(wantCpu));
    }
    if (b.le(base + 12, 4) != kMhDylib) {
        return notLoadable("not a dynamic library");
    }
    const std::uint64_t ncmds = b.le(base + 16, 4);
    const std::uint64_t end = base + size;
    struct Section {
        std::uint64_t addr, size, offset;
        bool zerofill;
    };
    std::vector<Section> sections;
    std::uint64_t symoff = 0, nsyms = 0, stroff = 0, strsize = 0;
    std::uint64_t iext = 0, next = 0, iundef = 0, nundef = 0;
    bool symtab = false, dysymtab = false;
    std::uint64_t at = base + 32;
    for (std::uint64_t i = 0; i < ncmds; ++i) {
        if (!b.has(at, 8) || at + 8 > end) {
            return notLoadable("truncated load commands");
        }
        const std::uint64_t cmd = b.le(at, 4), cmdsize = b.le(at + 4, 4);
        if (cmdsize < 8 || !b.has(at, cmdsize)) {
            return notLoadable("truncated load commands");
        }
        if (cmd == kLcSymtab && cmdsize >= 24) {
            symoff = b.le(at + 8, 4), nsyms = b.le(at + 12, 4);
            stroff = b.le(at + 16, 4), strsize = b.le(at + 20, 4);
            symtab = true;
        } else if (cmd == kLcDysymtab && cmdsize >= 80) {
            iext = b.le(at + 16, 4), next = b.le(at + 20, 4);
            iundef = b.le(at + 24, 4), nundef = b.le(at + 28, 4);
            dysymtab = true;
        } else if (cmd == kLcSegment64 && cmdsize >= 72) {
            const std::uint64_t nsects = b.le(at + 64, 4);
            for (std::uint64_t s = 0; s < nsects && 72 + 80 * (s + 1) <= cmdsize; ++s) {
                const std::uint64_t sec = at + 72 + 80 * s;
                const std::uint64_t type = b.le(sec + 64, 4) & 0xFF;
                sections.push_back(Section{b.le(sec + 32, 8), b.le(sec + 40, 8), b.le(sec + 48, 4),
                                           type == 1 || type == 0xC || type == 0x12});
            }
        }
        at += cmdsize;
    }
    if (!symtab) {
        return notLoadable("no symbol table");
    }
    if (!b.has(base + symoff, nsyms * 16) || !b.has(base + stroff, strsize)) {
        return notLoadable("the symbol table runs past the end of the file");
    }
    LibraryInfo info;
    const auto name = [&](std::uint64_t index) {
        const std::uint64_t strx = b.le(base + symoff + 16 * index, 4);
        return strx < strsize ? b.cstring(base + stroff + strx, base + stroff + strsize) : std::string();
    };
    const auto scan = [&](std::uint64_t first, std::uint64_t count) {
        for (std::uint64_t i = first; i < first + count && i < nsyms; ++i) {
            const std::uint64_t entry = base + symoff + 16 * i;
            const std::uint64_t type = b.le(entry + 4, 1);
            if ((type & 0xE0) != 0 || (type & 0x01) == 0) {
                continue; // a debugging entry, or not external
            }
            if ((type & 0x0E) == 0x0) {
                info.undefinedSymbols.push_back(stripUnderscore(name(i)));
            } else if ((type & 0x0E) == 0xE && (type & 0x10) == 0) {
                const std::string symbol = stripUnderscore(name(i));
                info.exportedSymbols.push_back(symbol);
                if (symbol == kAbiTagName) {
                    const std::uint64_t value = b.le(entry + 8, 8);
                    for (const Section &s : sections) {
                        if (!s.zerofill && value >= s.addr && value - s.addr < s.size) {
                            info.abiTag = tagFrom(b, base + s.offset + (value - s.addr));
                        }
                    }
                }
            }
        }
    };
    if (dysymtab) {
        scan(iext, next);
        scan(iundef, nundef);
    } else {
        scan(0, nsyms);
    }
    return info;
}

LibraryInfo readMachOFile(const Bytes &b, std::string_view hostAbi) {
    const std::string arch = hostArch(hostAbi);
    const std::uint64_t want = arch == "arm64" ? kCpuArm64 : arch == "x86_64" ? kCpuX86_64 : 0;
    if (!hostIs(hostAbi, "macos")) {
        return notLoadable("a Mach-O file; this host loads " +
                           std::string(hostIs(hostAbi, "windows") ? "PE" : "ELF") + " libraries");
    }
    if (b.be(0, 4) == kFatMagic) {
        const std::uint64_t n = b.be(4, 4);
        for (std::uint64_t i = 0; i < n && b.has(8 + 20 * i, 20); ++i) {
            if (b.be(8 + 20 * i, 4) == want) {
                const std::uint64_t offset = b.be(8 + 20 * i + 8, 4), size = b.be(8 + 20 * i + 12, 4);
                if (!b.has(offset, size)) {
                    return notLoadable("a universal file whose " + arch + " part runs past its end");
                }
                return readMachO(b, offset, size, want);
            }
        }
        return notLoadable("a universal file with no " + arch + " part");
    }
    return readMachO(b, 0, b.size(), want);
}

// ---- ELF ----

LibraryInfo readElf(const Bytes &b, std::string_view hostAbi) {
    if (!hostIs(hostAbi, "linux")) {
        return notLoadable("an ELF file; this host loads " +
                           std::string(hostIs(hostAbi, "windows") ? "PE" : "Mach-O") + " libraries");
    }
    if (!b.has(0, 64) || b.le(4, 1) != 2 || b.le(5, 1) != 1) {
        return notLoadable("not a little-endian 64-bit ELF file");
    }
    if (b.le(16, 2) != 3) {
        return notLoadable("not a shared object");
    }
    const std::uint64_t machine = b.le(18, 2);
    const std::string arch = hostArch(hostAbi);
    if ((arch == "x86_64" && machine != 62) || (arch == "arm64" && machine != 183)) {
        return notLoadable(std::string("built for ") + (machine == 62 ? "x86_64" : machine == 183 ? "arm64"
                                                                                                 : "an unknown architecture") +
                           ", this host is " + arch);
    }
    const std::uint64_t shoff = b.le(40, 8), shentsize = b.le(58, 2), shnum = b.le(60, 2);
    if (shentsize < 64 || !b.has(shoff, shentsize * shnum)) {
        return notLoadable("the section table runs past the end of the file");
    }
    const auto sh = [&](std::uint64_t i, std::uint64_t field, int size) {
        return b.le(shoff + shentsize * i + field, size);
    };
    LibraryInfo info;
    for (std::uint64_t i = 0; i < shnum; ++i) {
        if (sh(i, 4, 4) != 11) { // SHT_DYNSYM
            continue;
        }
        const std::uint64_t off = sh(i, 24, 8), size = sh(i, 32, 8), link = sh(i, 40, 4);
        if (link >= shnum || !b.has(off, size)) {
            return notLoadable("the dynamic symbol table runs past the end of the file");
        }
        const std::uint64_t stroff = sh(link, 24, 8), strsize = sh(link, 32, 8);
        for (std::uint64_t s = 1; s < size / 24; ++s) {
            const std::uint64_t e = off + 24 * s;
            const std::uint64_t bind = b.le(e + 4, 1) >> 4, other = b.le(e + 5, 1), shndx = b.le(e + 6, 2);
            if (bind != 1 && bind != 2) {
                continue; // local
            }
            const std::uint64_t strx = b.le(e, 4);
            const std::string name = strx < strsize ? b.cstring(stroff + strx, stroff + strsize) : "";
            if (shndx == 0) {
                info.undefinedSymbols.push_back(name);
            } else if ((other & 3) == 0) {
                info.exportedSymbols.push_back(name);
                if (name == kAbiTagName && shndx < shnum) {
                    const std::uint64_t value = b.le(e + 8, 8);
                    const std::uint64_t addr = sh(shndx, 16, 8), offset = sh(shndx, 24, 8);
                    if (sh(shndx, 4, 4) != 8 && value >= addr) { // not SHT_NOBITS
                        info.abiTag = tagFrom(b, offset + (value - addr));
                    }
                }
            }
        }
        return info;
    }
    return notLoadable("no dynamic symbol table");
}

// ---- PE ----

LibraryInfo readPe(const Bytes &b, std::string_view hostAbi) {
    if (!hostIs(hostAbi, "windows")) {
        return notLoadable("a PE file; this host loads " +
                           std::string(hostIs(hostAbi, "macos") ? "Mach-O" : "ELF") + " libraries");
    }
    if (!b.has(0x3C, 4)) {
        return notLoadable("not a PE file");
    }
    const std::uint64_t pe = b.le(0x3C, 4);
    if (!b.has(pe, 24) || b.le(pe, 4) != 0x00004550u) {
        return notLoadable("not a PE file");
    }
    const std::uint64_t machine = b.le(pe + 4, 2), nsec = b.le(pe + 6, 2), optsize = b.le(pe + 20, 2);
    const std::string arch = hostArch(hostAbi);
    if ((arch == "x86_64" && machine != 0x8664) || (arch == "arm64" && machine != 0xAA64)) {
        return notLoadable("built for another architecture, this host is " + arch);
    }
    if ((b.le(pe + 22, 2) & 0x2000) == 0) {
        return notLoadable("not a DLL");
    }
    const std::uint64_t opt = pe + 24;
    // The optional header, with the two data directories read below (export at 112, import at
    // 120), lies wholly in the file before any of it is read.
    if (optsize < 112 + 16 || !b.has(opt, optsize) || b.le(opt, 2) != 0x20B) {
        return notLoadable(optsize >= 112 + 16 && !b.has(opt, optsize) ? "the optional header runs past the end of the file"
                                                                       : "not a PE32+ file");
    }
    const std::uint64_t directories = b.le(opt + 108, 4); // how many of them the header declares
    const std::uint64_t secs = opt + optsize;
    const auto offsetOf = [&](std::uint64_t rva) -> std::optional<std::uint64_t> {
        for (std::uint64_t i = 0; i < nsec && b.has(secs + 40 * i, 40); ++i) {
            const std::uint64_t s = secs + 40 * i;
            const std::uint64_t va = b.le(s + 12, 4), raw = b.le(s + 16, 4), ptr = b.le(s + 20, 4);
            if (rva >= va && rva - va < raw) {
                return ptr + (rva - va);
            }
        }
        return std::nullopt;
    };
    LibraryInfo info;
    const std::uint64_t exportRva = directories >= 1 ? b.le(opt + 112, 4) : 0;
    if (const auto e = exportRva ? offsetOf(exportRva) : std::nullopt; e && b.has(*e, 40)) {
        const std::uint64_t nnames = b.le(*e + 24, 4), funcs = b.le(*e + 28, 4);
        const std::uint64_t names = b.le(*e + 32, 4), ords = b.le(*e + 36, 4);
        for (std::uint64_t i = 0; i < nnames; ++i) {
            const auto n = offsetOf(names + 4 * i), o = offsetOf(ords + 2 * i);
            if (!n || !o || !b.has(*n, 4) || !b.has(*o, 2)) {
                break;
            }
            const auto s = offsetOf(b.le(*n, 4));
            const std::string name = s ? b.cstring(*s, b.size()) : "";
            info.exportedSymbols.push_back(name);
            if (name == kAbiTagName) {
                if (const auto f = offsetOf(funcs + 4 * b.le(*o, 2)); f && b.has(*f, 4)) {
                    if (const auto t = offsetOf(b.le(*f, 4))) {
                        info.abiTag = tagFrom(b, *t);
                    }
                }
            }
        }
    }
    const std::uint64_t importRva = directories >= 2 ? b.le(opt + 120, 4) : 0;
    for (auto d = importRva ? offsetOf(importRva) : std::nullopt; d && b.has(*d, 20); *d += 20) {
        const std::uint64_t lookup = b.le(*d, 4) ? b.le(*d, 4) : b.le(*d + 16, 4);
        if (lookup == 0) {
            break;
        }
        for (std::uint64_t i = 0;; ++i) {
            const auto t = offsetOf(lookup + 8 * i);
            if (!t || !b.has(*t, 8) || b.le(*t, 8) == 0) {
                break;
            }
            const std::uint64_t entry = b.le(*t, 8);
            if (entry >> 63) {
                info.undefinedSymbols.push_back("#" + std::to_string(entry & 0xFFFF));
            } else if (const auto h = offsetOf(entry & 0x7FFFFFFF)) {
                info.undefinedSymbols.push_back(b.cstring(*h + 2, b.size()));
            }
        }
    }
    return info;
}

// ---- module.json ----

// A JSON object of string members, as module.json is; anything else is refused by the field it
// breaks at.
class Json {
public:
    explicit Json(std::string_view text) : text_(text) {}

    bool object(std::map<std::string, std::string> &out, std::string &bad) {
        skip();
        if (!eat('{')) {
            bad = "not a JSON object";
            return false;
        }
        skip();
        if (eat('}')) {
            return atEnd(bad);
        }
        for (;;) {
            std::string key, value;
            skip();
            if (!string(key)) {
                bad = "not a JSON object";
                return false;
            }
            skip();
            if (!eat(':')) {
                bad = "not a JSON object";
                return false;
            }
            skip();
            if (!string(value)) {
                bad = key + " is not a string";
                return false;
            }
            if (out.count(key) != 0) {
                bad = key + " appears twice";
                return false;
            }
            out[key] = value;
            skip();
            if (eat('}')) {
                return atEnd(bad);
            }
            if (!eat(',')) {
                bad = "not a JSON object";
                return false;
            }
        }
    }

private:
    void skip() {
        while (pos_ < text_.size() && (text_[pos_] == ' ' || text_[pos_] == '\n' || text_[pos_] == '\r' ||
                                       text_[pos_] == '\t')) {
            ++pos_;
        }
    }
    bool eat(char c) {
        if (pos_ < text_.size() && text_[pos_] == c) {
            ++pos_;
            return true;
        }
        return false;
    }
    bool atEnd(std::string &bad) {
        skip();
        if (pos_ != text_.size()) {
            bad = "not a JSON object";
            return false;
        }
        return true;
    }
    // A string of printable ASCII; escapes other than \" and \\ are refused (module.json needs none).
    bool string(std::string &out) {
        if (!eat('"')) {
            return false;
        }
        while (pos_ < text_.size()) {
            const char c = text_[pos_++];
            if (c == '"') {
                return true;
            }
            if (c == '\\') {
                if (pos_ >= text_.size() || (text_[pos_] != '"' && text_[pos_] != '\\')) {
                    return false;
                }
                out += text_[pos_++];
            } else if (static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) > 0x7E) {
                return false;
            } else {
                out += c;
            }
        }
        return false;
    }

    std::string_view text_;
    std::size_t pos_ = 0;
};

std::optional<std::string> readFile(const std::filesystem::path &path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

} // namespace

LibraryInfo readLibrary(std::string_view bytes, std::string_view hostAbi) {
    const Bytes b(bytes);
    if (!b.has(0, 4)) {
        return notLoadable("too short to be a library");
    }
    if (b.le(0, 4) == kMhMagic64 || b.be(0, 4) == kFatMagic || b.le(0, 4) == 0xFEEDFACEu) {
        return readMachOFile(b, hostAbi);
    }
    if (bytes.substr(0, 4) == std::string_view("\x7F" "ELF", 4)) {
        return readElf(b, hostAbi);
    }
    if (bytes.substr(0, 2) == "MZ") {
        return readPe(b, hostAbi);
    }
    return notLoadable("not a Mach-O, ELF or PE file");
}

ManifestRead parseManifest(std::string_view json) {
    ManifestRead read;
    std::map<std::string, std::string> fields;
    std::string bad;
    if (!Json(json).object(fields, bad)) {
        read.error = bad;
        return read;
    }
    for (const char *key : {"name", "source_sha256", "translator_version"}) {
        if (fields.count(key) == 0) {
            read.error = key; // ModuleContract.md §5: the detail is the field
            return read;
        }
    }
    const std::string &sha = fields["source_sha256"];
    if (sha.size() != 64 || sha.find_first_not_of("0123456789abcdef") != std::string::npos) {
        read.error = "source_sha256";
        return read;
    }
    if (fields["name"].empty()) {
        read.error = "name";
        return read;
    }
    if (fields["translator_version"].empty()) {
        read.error = "translator_version";
        return read;
    }
    read.manifest = ModuleManifest{fields["name"], sha, fields["translator_version"]};
    return read;
}

ModuleImage readModuleImage(const std::filesystem::path &library) {
    ModuleImage image;
    const std::filesystem::path manifest = library.parent_path() / "module.json";
    if (const auto text = readFile(manifest)) {
        ManifestRead read = parseManifest(*text);
        image.manifest = std::move(read.manifest);
        image.manifestError = std::move(read.error);
    }
    const auto bytes = readFile(library);
    if (!bytes) {
        image.notLoadable = "cannot be read";
        return image;
    }
    LibraryInfo info = readLibrary(*bytes);
    image.notLoadable = std::move(info.notLoadable);
    image.abiTag = info.abiTag;
    image.undefinedSymbols = std::move(info.undefinedSymbols);
    image.exportedSymbols = std::move(info.exportedSymbols);
    return image;
}

} // namespace sfcop::scripts
