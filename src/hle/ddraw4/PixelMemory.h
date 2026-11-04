// Where a surface's pixels live while the game can lock them. Lock() hands the game a pointer
// into this memory, so the guest layer allocates it where the game can address it; tests use the
// host heap.
#pragma once

#include <cstddef>
#include <cstdint>

namespace sfcop::hle::ddraw4 {

class PixelMemory {
public:
    virtual ~PixelMemory() = default;

    // A zero-filled block of `bytes`, or nullptr if memory is exhausted.
    virtual std::uint8_t *Allocate(std::size_t bytes) = 0;

    // Returns a block from Allocate(); nullptr does nothing.
    virtual void Free(std::uint8_t *block) = 0;
};

// PixelMemory on the host heap.
class HeapPixelMemory final : public PixelMemory {
public:
    std::uint8_t *Allocate(std::size_t bytes) override;
    void Free(std::uint8_t *block) override;
};

} // namespace sfcop::hle::ddraw4
