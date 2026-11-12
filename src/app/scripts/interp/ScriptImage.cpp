// The .scr's image laid out at a guest base (ScriptImage.h).
#include "app/scripts/interp/ScriptImage.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <utility>

namespace sfcop::scripts::interp {

namespace {

constexpr std::uint16_t kMachineI386 = 0x014C;
constexpr std::uint16_t kFileDll = 0x2000;
constexpr std::uint16_t kPe32Magic = 0x010B;
constexpr std::uint32_t kGranule = 0x10000u;
constexpr std::uint32_t kLargestImage = 0x01000000u; // 16 MB; the shipped scripts need 0.5 MB
constexpr unsigned kRelocAbsolute = 0;
constexpr unsigned kRelocHighLow = 3;
constexpr std::uint32_t kSectionCode = 0x00000020u;
constexpr std::uint32_t kSectionExecute = 0x20000000u;
constexpr std::uint32_t kSectionWrite = 0x80000000u;

// Directory entries.
constexpr unsigned kDirExport = 0;
constexpr unsigned kDirImport = 1;
constexpr unsigned kDirReloc = 5;
constexpr unsigned kDirTls = 9;

struct Failure {
    std::string why;
};

std::string hex(std::uint32_t value) {
    char text[16];
    std::snprintf(text, sizeof text, "0x%X", value);
    return text;
}

// Reads the file with every offset checked.
class File {
public:
    explicit File(const std::vector<std::uint8_t> &data) : data_(data) {}

    std::uint32_t u32(std::uint64_t offset) const {
        need(offset, 4);
        std::uint32_t v;
        std::memcpy(&v, data_.data() + offset, 4);
        return v;
    }
    std::uint16_t u16(std::uint64_t offset) const {
        need(offset, 2);
        std::uint16_t v;
        std::memcpy(&v, data_.data() + offset, 2);
        return v;
    }
    void need(std::uint64_t offset, std::uint64_t size) const {
        if (offset + size > data_.size()) {
            throw Failure{"it is truncated: " + std::to_string(size) + " bytes at file offset " +
                          hex(static_cast<std::uint32_t>(offset)) + " are past its end"};
        }
    }
    std::size_t size() const { return data_.size(); }
    const std::uint8_t *at(std::uint64_t offset) const { return data_.data() + offset; }

private:
    const std::vector<std::uint8_t> &data_;
};

// The image as laid out: reads by rva, with every range checked against the image.
class Image {
public:
    explicit Image(std::vector<std::uint8_t> &bytes) : bytes_(bytes) {}

    std::uint32_t u32(std::uint32_t rva) const {
        need(rva, 4);
        std::uint32_t v;
        std::memcpy(&v, bytes_.data() + rva, 4);
        return v;
    }
    std::uint16_t u16(std::uint32_t rva) const {
        need(rva, 2);
        std::uint16_t v;
        std::memcpy(&v, bytes_.data() + rva, 2);
        return v;
    }
    void put32(std::uint32_t rva, std::uint32_t v) {
        need(rva, 4);
        std::memcpy(bytes_.data() + rva, &v, 4);
    }
    std::string cstring(std::uint32_t rva) const {
        need(rva, 1);
        const std::size_t limit = std::min<std::size_t>(bytes_.size() - rva, 512);
        const auto *start = bytes_.data() + rva;
        const auto *end = static_cast<const std::uint8_t *>(std::memchr(start, 0, limit));
        if (end == nullptr) {
            throw Failure{"the string at rva " + hex(rva) + " is not terminated"};
        }
        return std::string(reinterpret_cast<const char *>(start), static_cast<std::size_t>(end - start));
    }
    void need(std::uint64_t rva, std::uint64_t size) const {
        if (rva + size > bytes_.size()) {
            throw Failure{"rva " + hex(static_cast<std::uint32_t>(rva)) + " (+" + std::to_string(size) +
                          ") is outside the image"};
        }
    }

    std::uint32_t size() const { return static_cast<std::uint32_t>(bytes_.size()); }

private:
    std::vector<std::uint8_t> &bytes_;
};

std::string upper(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return text;
}

void readImports(Image &image, std::uint32_t directory, ScriptImage &out) {
    if (directory == 0) {
        return;
    }
    for (std::uint32_t descriptor = directory;; descriptor += 20) {
        const std::uint32_t lookupTable = image.u32(descriptor);
        const std::uint32_t nameRva = image.u32(descriptor + 12);
        const std::uint32_t addressTable = image.u32(descriptor + 16);
        if (nameRva == 0 && addressTable == 0) {
            return;
        }
        const std::string dll = upper(image.cstring(nameRva));
        const std::uint32_t lookup = lookupTable != 0 ? lookupTable : addressTable;
        for (std::uint32_t i = 0;; ++i) {
            const std::uint32_t entry = image.u32(lookup + 4 * i);
            if (entry == 0) {
                break;
            }
            const std::string function = (entry & 0x80000000u) != 0
                                             ? "#" + std::to_string(entry & 0xFFFFu)
                                             : image.cstring(entry + 2);
            out.imports.push_back({addressTable + 4 * i, dll + "!" + function});
        }
    }
}

void readExports(Image &image, std::uint32_t directory, std::uint32_t directorySize, ScriptImage &out) {
    if (directory == 0) {
        return;
    }
    const std::uint32_t functions = image.u32(directory + 20);
    const std::uint32_t names = image.u32(directory + 24);
    const std::uint32_t functionTable = image.u32(directory + 28);
    const std::uint32_t nameTable = image.u32(directory + 32);
    const std::uint32_t ordinalTable = image.u32(directory + 36);
    for (std::uint32_t i = 0; i < names; ++i) {
        const std::string name = image.cstring(image.u32(nameTable + 4 * i));
        const std::uint32_t ordinal = image.u16(ordinalTable + 2 * i);
        if (ordinal >= functions) {
            throw Failure{"its export " + name + " names a function past its export table"};
        }
        const std::uint32_t target = image.u32(functionTable + 4 * ordinal);
        if (target >= directory && target < directory + directorySize) {
            throw Failure{"its export " + name + " is forwarded, which a mission never does"};
        }
        if (target >= image.size()) {
            throw Failure{"its export " + name + " lies outside the image"};
        }
        if (name == "MissionSpecEntry") {
            out.missionSpecEntry = out.base + target;
        } else if (name == "ScriptEntry") {
            out.scriptEntry = out.base + target;
        }
    }
}

void relocate(Image &image, std::uint32_t directory, std::uint32_t directorySize, std::uint32_t delta,
              std::vector<std::uint32_t> &pointers) {
    for (std::uint32_t block = directory; block < directory + directorySize;) {
        const std::uint32_t page = image.u32(block);
        const std::uint32_t blockSize = image.u32(block + 4);
        if (blockSize < 8 || blockSize % 2 != 0) {
            throw Failure{"its relocation block at rva " + hex(block) + " has size " +
                          std::to_string(blockSize)};
        }
        for (std::uint32_t k = 8; k < blockSize; k += 2) {
            const std::uint16_t word = image.u16(block + k);
            const unsigned kind = word >> 12;
            const std::uint32_t at = page + (word & 0x0FFFu);
            if (kind == kRelocAbsolute) {
                continue;
            }
            if (kind != kRelocHighLow) {
                throw Failure{"it has a relocation of type " + std::to_string(kind) + " at rva " + hex(at)};
            }
            image.put32(at, image.u32(at) + delta);
            pointers.push_back(at);
        }
        block += blockSize;
    }
}

ScriptImage layOut(const File &file, std::uint32_t base) {
    if (file.size() < 0x40 || file.at(0)[0] != 'M' || file.at(0)[1] != 'Z') {
        throw Failure{"it is not a Windows program (no MZ header)"};
    }
    const std::uint32_t pe = file.u32(0x3C);
    file.need(pe, 24);
    if (std::memcmp(file.at(pe), "PE\0\0", 4) != 0) {
        throw Failure{"it is not a Windows program (no PE signature)"};
    }
    const std::uint16_t machine = file.u16(pe + 4);
    const std::uint16_t sectionCount = file.u16(pe + 6);
    const std::uint16_t optionalSize = file.u16(pe + 20);
    const std::uint16_t characteristics = file.u16(pe + 22);
    if (machine != kMachineI386) {
        throw Failure{"it is built for machine " + hex(machine) + ", not i386"};
    }
    if ((characteristics & kFileDll) == 0) {
        throw Failure{"it is not a DLL"};
    }
    const std::uint64_t optional = pe + 24;
    if (file.u16(optional) != kPe32Magic) {
        throw Failure{"it is not a 32-bit program (no PE32 optional header)"};
    }
    ScriptImage out;
    const std::uint32_t entryRva = file.u32(optional + 16);
    out.linkedBase = file.u32(optional + 28);
    const std::uint32_t imageSize = file.u32(optional + 56);
    const std::uint32_t headersSize = file.u32(optional + 60);
    const std::uint32_t directoryCount = file.u32(optional + 92);
    if (imageSize == 0 || imageSize > kLargestImage) {
        throw Failure{"its image size " + hex(imageSize) + " is not that of a mission"};
    }
    if (headersSize > imageSize || headersSize > file.size()) {
        throw Failure{"its headers run past the end of the file"};
    }
    auto directory = [&](unsigned index) -> std::pair<std::uint32_t, std::uint32_t> {
        if (index >= directoryCount || 96u + 8u * index + 8u > optionalSize) {
            return {0, 0};
        }
        return {file.u32(optional + 96 + 8 * index), file.u32(optional + 100 + 8 * index)};
    };
    out.base = base;
    out.size = (imageSize + kGranule - 1) / kGranule * kGranule;
    out.bytes.assign(out.size, 0);
    std::memcpy(out.bytes.data(), file.at(0), headersSize);
    const std::uint64_t sectionTable = optional + optionalSize;
    for (std::uint32_t i = 0; i < sectionCount; ++i) {
        const std::uint64_t at = sectionTable + 40u * i;
        file.need(at, 40);
        ScriptSection section;
        const std::uint32_t virtualSize = file.u32(at + 8);
        section.rva = file.u32(at + 12);
        const std::uint32_t rawSize = file.u32(at + 16);
        const std::uint32_t rawOffset = file.u32(at + 20);
        const std::uint32_t flags = file.u32(at + 36);
        section.size = virtualSize != 0 ? virtualSize : rawSize;
        section.executable = (flags & (kSectionCode | kSectionExecute)) != 0;
        section.writable = (flags & kSectionWrite) != 0;
        if (std::uint64_t{section.rva} + section.size > imageSize) {
            throw Failure{"its section " + std::to_string(i + 1) + " lies outside the image"};
        }
        const std::uint32_t copied = std::min(rawSize, section.size);
        if (copied != 0) {
            file.need(rawOffset, copied);
            std::memcpy(out.bytes.data() + section.rva, file.at(rawOffset), copied);
        }
        out.sections.push_back(section);
    }
    Image image(out.bytes);
    if (directory(kDirTls).first != 0) {
        throw Failure{"it has thread-local storage, which a mission never has"};
    }
    readImports(image, directory(kDirImport).first, out);
    readExports(image, directory(kDirExport).first, directory(kDirExport).second, out);
    const auto reloc = directory(kDirReloc);
    if (base != out.linkedBase) {
        if (reloc.first == 0) {
            throw Failure{"it has no base relocations, so it cannot be moved from " + hex(out.linkedBase)};
        }
        relocate(image, reloc.first, reloc.second, base - out.linkedBase, out.pointers);
        image.put32(static_cast<std::uint32_t>(optional + 28), base);
    }
    if (entryRva != 0) {
        if (entryRva >= imageSize) {
            throw Failure{"its entry point lies outside the image"};
        }
        out.entry = base + entryRva;
    }
    return out;
}

} // namespace

ScriptImageResult readScriptImage(const std::vector<std::uint8_t> &file, std::uint32_t base) {
    ScriptImageResult result;
    if (base % kGranule != 0) {
        result.error = "the base " + hex(base) + " is not on a 64 KB boundary";
        return result;
    }
    try {
        result.image = layOut(File(file), base);
        result.ok = true;
    } catch (const Failure &failure) {
        result.error = failure.why;
    }
    return result;
}

} // namespace sfcop::scripts::interp
