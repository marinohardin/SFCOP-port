// Little-endian byte writing and bounds-checked reading, for the LAN protocol (Wire.h), the
// DirectPlay address format (Address.h) and the guest layouts (Layout.h). A reader that runs
// past its end stays failed and returns zeros, so a decoder checks Ok() once at the end.
#pragma once

#include "hle/dplay/DplayDefs.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

namespace sfcop::hle::dplay {

class ByteWriter {
public:
    void U8(std::uint8_t v) { out_.push_back(v); }
    void U16(std::uint16_t v) {
        U8(static_cast<std::uint8_t>(v));
        U8(static_cast<std::uint8_t>(v >> 8));
    }
    void U32(std::uint32_t v) {
        U16(static_cast<std::uint16_t>(v));
        U16(static_cast<std::uint16_t>(v >> 16));
    }
    void Raw(const void *data, std::size_t bytes) {
        const auto *p = static_cast<const std::uint8_t *>(data);
        out_.insert(out_.end(), p, p + bytes);
    }
    void Raw(const std::vector<std::uint8_t> &bytes) { Raw(bytes.data(), bytes.size()); }
    void Id(const Guid &g) { Raw(g.bytes.data(), g.bytes.size()); }
    // A string: its length as a u16, then its bytes; 0xFFFF for an absent one.
    void Str(const std::optional<std::string> &s) {
        if (!s) {
            U16(0xFFFF);
            return;
        }
        U16(static_cast<std::uint16_t>(s->size()));
        Raw(s->data(), s->size());
    }
    // A byte string: its length as a u32, then its bytes.
    void Bytes(const std::vector<std::uint8_t> &b) {
        U32(static_cast<std::uint32_t>(b.size()));
        Raw(b);
    }
    // Overwrites a u32 written earlier.
    void PatchU32(std::size_t at, std::uint32_t v) {
        for (int i = 0; i < 4; ++i) {
            out_[at + i] = static_cast<std::uint8_t>(v >> (8 * i));
        }
    }
    std::size_t Size() const { return out_.size(); }
    std::vector<std::uint8_t> &Out() { return out_; }
    std::vector<std::uint8_t> Take() { return std::move(out_); }

private:
    std::vector<std::uint8_t> out_;
};

class ByteReader {
public:
    ByteReader(const std::uint8_t *data, std::size_t bytes) : data_(data), size_(bytes) {}
    explicit ByteReader(const std::vector<std::uint8_t> &bytes)
        : data_(bytes.data()), size_(bytes.size()) {}

    bool Ok() const { return ok_; }
    std::size_t Left() const { return ok_ ? size_ - at_ : 0; }
    bool AtEnd() const { return ok_ && at_ == size_; }

    std::uint8_t U8() {
        if (!need(1)) {
            return 0;
        }
        return data_[at_++];
    }
    std::uint16_t U16() {
        std::uint16_t lo = U8();
        return static_cast<std::uint16_t>(lo | U8() << 8);
    }
    std::uint32_t U32() {
        std::uint32_t lo = U16();
        return lo | static_cast<std::uint32_t>(U16()) << 16;
    }
    Guid Id() {
        Guid g;
        if (need(16)) {
            std::memcpy(g.bytes.data(), data_ + at_, 16);
            at_ += 16;
        }
        return g;
    }
    std::optional<std::string> Str() {
        std::uint16_t n = U16();
        if (n == 0xFFFF || !ok_) {
            return std::nullopt;
        }
        if (!need(n)) {
            return std::nullopt;
        }
        std::string s(reinterpret_cast<const char *>(data_ + at_), n);
        at_ += n;
        return s;
    }
    std::vector<std::uint8_t> Bytes() { return Raw(U32()); }
    std::vector<std::uint8_t> Raw(std::size_t n) {
        if (!need(n)) {
            return {};
        }
        std::vector<std::uint8_t> v(data_ + at_, data_ + at_ + n);
        at_ += n;
        return v;
    }
    std::vector<std::uint8_t> Rest() { return Raw(Left()); }

private:
    bool need(std::size_t n) {
        if (!ok_ || size_ - at_ < n) {
            ok_ = false;
            return false;
        }
        return true;
    }

    const std::uint8_t *data_;
    std::size_t size_;
    std::size_t at_ = 0;
    bool ok_ = true;
};

} // namespace sfcop::hle::dplay
