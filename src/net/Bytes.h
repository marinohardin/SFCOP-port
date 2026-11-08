// Little-endian reading and writing for the network formats of PROTOCOL.md and RENDEZVOUS.md.
// A ByteReader never reads past its end: a read that does not fit sets the failed flag and
// returns zero, so a decoder reads every field and checks Ok() once at the end.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace sfcop::net {

class ByteReader {
public:
    ByteReader(const std::uint8_t *data, std::size_t bytes) : data_(data), size_(bytes) {}

    std::uint8_t U8() { return static_cast<std::uint8_t>(read(1)); }
    std::uint16_t U16() { return static_cast<std::uint16_t>(read(2)); }
    std::uint32_t U32() { return static_cast<std::uint32_t>(read(4)); }
    std::uint64_t U64() { return read(8); }

    // Copies `bytes` bytes into `out`; on a short read, fails and leaves `out` zeroed.
    void Bytes(std::uint8_t *out, std::size_t bytes) {
        if (!fits(bytes)) {
            std::memset(out, 0, bytes);
            return;
        }
        std::memcpy(out, data_ + offset_, bytes);
        offset_ += bytes;
    }

    void Skip(std::size_t bytes) {
        if (fits(bytes)) {
            offset_ += bytes;
        }
    }

    std::size_t Remaining() const { return failed_ ? 0 : size_ - offset_; }
    const std::uint8_t *Here() const { return data_ + offset_; }
    bool Ok() const { return !failed_; }
    // True when every byte was read and none was missing.
    bool Done() const { return !failed_ && offset_ == size_; }

private:
    const std::uint8_t *data_;
    std::size_t size_;
    std::size_t offset_ = 0;
    bool failed_ = false;

    bool fits(std::size_t bytes) {
        if (failed_ || size_ - offset_ < bytes) {
            failed_ = true;
            return false;
        }
        return true;
    }

    std::uint64_t read(std::size_t bytes) {
        if (!fits(bytes)) {
            return 0;
        }
        std::uint64_t value = 0;
        for (std::size_t i = 0; i < bytes; ++i) {
            value |= static_cast<std::uint64_t>(data_[offset_ + i]) << (8 * i);
        }
        offset_ += bytes;
        return value;
    }
};

class ByteWriter {
public:
    explicit ByteWriter(std::vector<std::uint8_t> &out) : out_(out) {}

    void U8(std::uint8_t value) { write(value, 1); }
    void U16(std::uint16_t value) { write(value, 2); }
    void U32(std::uint32_t value) { write(value, 4); }
    void U64(std::uint64_t value) { write(value, 8); }
    void Bytes(const std::uint8_t *data, std::size_t bytes) {
        out_.insert(out_.end(), data, data + bytes);
    }
    void Zeros(std::size_t bytes) { out_.insert(out_.end(), bytes, 0); }

private:
    std::vector<std::uint8_t> &out_;

    void write(std::uint64_t value, std::size_t bytes) {
        for (std::size_t i = 0; i < bytes; ++i) {
            out_.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
        }
    }
};

// The first four bytes of a datagram as a little-endian u32, or 0 when it is shorter.
inline std::uint32_t datagramMagic(const std::uint8_t *data, std::size_t bytes) {
    ByteReader reader(data, bytes);
    std::uint32_t magic = reader.U32();
    return reader.Ok() ? magic : 0;
}

} // namespace sfcop::net
