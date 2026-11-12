#include "hle/ddraw4/ChangedPixels.h"

#include <doctest/doctest.h>

#include <array>
#include <cstring>
#include <vector>

namespace sfcop::hle::ddraw4 {

namespace {

// A 4 by 3 surface of 16-bit pixels, rows packed (8 bytes apart).
using Pixels16 = std::array<std::uint16_t, 12>;

const std::uint8_t *bytes(const Pixels16 &pixels) {
    return reinterpret_cast<const std::uint8_t *>(pixels.data());
}

std::vector<std::uint16_t> words(const std::vector<std::uint8_t> &packed) {
    std::vector<std::uint16_t> out(packed.size() / 2);
    std::memcpy(out.data(), packed.data(), packed.size());
    return out;
}

} // namespace

TEST_CASE("an edit that changes nothing marks nothing") {
    ChangedPixels changes;
    changes.Reset(4, 3);
    const Pixels16 before{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
    changes.Compare(RectL{0, 0, 4, 3}, bytes(before), 8, bytes(before), 8, 2);
    CHECK(changes.Empty());
    CHECK_FALSE(changes.Whole());
}

TEST_CASE("the marked area is the smallest rectangle around the changed pixels") {
    ChangedPixels changes;
    changes.Reset(4, 3);
    const Pixels16 before{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
    Pixels16 after = before;
    after[1] = 0x50;  // x 1, y 0
    after[10] = 0x60; // x 2, y 2
    changes.Compare(RectL{0, 0, 4, 3}, bytes(before), 8, bytes(after), 8, 2);
    CHECK_FALSE(changes.Empty());
    CHECK(changes.Area().left == 1);
    CHECK(changes.Area().top == 0);
    CHECK(changes.Area().right == 3);
    CHECK(changes.Area().bottom == 3);
    CHECK_FALSE(changes.Whole());

    // The patch keeps the two changed pixels; every other pixel of the area is the key, 0 here
    // (no changed pixel is 0).
    std::vector<std::uint8_t> patch;
    const std::uint32_t key = changes.Patch(bytes(after), 8, 2, 0xFFFF, patch);
    CHECK(key == 0);
    CHECK(words(patch) == std::vector<std::uint16_t>{0x50, 0, 0, 0, 0, 0x60});
}

TEST_CASE("the key is the lowest value no changed pixel has") {
    ChangedPixels changes;
    changes.Reset(4, 3);
    const Pixels16 before{9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9};
    Pixels16 after = before;
    after[0] = 0;
    after[1] = 1;
    after[2] = 3;
    changes.Compare(RectL{0, 0, 4, 3}, bytes(before), 8, bytes(after), 8, 2);
    std::vector<std::uint8_t> patch;
    CHECK(changes.Patch(bytes(after), 8, 2, 0xFFFF, patch) == 2);
    CHECK(words(patch) == std::vector<std::uint16_t>{0, 1, 3});
    CHECK(changes.Whole()); // one row, every pixel of it changed
}

TEST_CASE("a partial edit compares only its own area, at its own pitch") {
    ChangedPixels changes;
    changes.Reset(4, 3);
    // The copy kept at Lock holds only the 2 by 2 area at x 2, y 1, rows 4 bytes apart.
    const std::array<std::uint16_t, 4> kept{7, 8, 11, 12};
    Pixels16 after{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
    after[0] = 0x77; // outside the locked area: not the lock's change, not marked
    after[11] = 0x40;
    changes.Compare(RectL{2, 1, 4, 3}, reinterpret_cast<const std::uint8_t *>(kept.data()), 4,
                    bytes(after) + (1 * 8) + (2 * 2), 8, 2);
    CHECK(changes.Area().left == 3);
    CHECK(changes.Area().top == 2);
    CHECK(changes.Area().right == 4);
    CHECK(changes.Area().bottom == 3);
    CHECK(changes.Whole());
}

TEST_CASE("marks gather over several edits until cleared") {
    ChangedPixels changes;
    changes.Reset(4, 3);
    const Pixels16 before{};
    Pixels16 first = before;
    first[0] = 1;
    changes.Compare(RectL{0, 0, 4, 3}, bytes(before), 8, bytes(first), 8, 2);
    Pixels16 second = first;
    second[11] = 2;
    changes.Compare(RectL{0, 0, 4, 3}, bytes(first), 8, bytes(second), 8, 2);
    CHECK(changes.Area().left == 0);
    CHECK(changes.Area().top == 0);
    CHECK(changes.Area().right == 4);
    CHECK(changes.Area().bottom == 3);
    CHECK_FALSE(changes.Whole());
    changes.Clear();
    CHECK(changes.Empty());
    CHECK(changes.SizedFor(4, 3));
    CHECK_FALSE(changes.SizedFor(8, 3));
}

TEST_CASE("32-bit pixels take a key within the value mask") {
    ChangedPixels changes;
    changes.Reset(2, 1);
    const std::array<std::uint32_t, 2> before{5, 5};
    const std::array<std::uint32_t, 2> after{0xFF000000, 5};
    const auto *from = reinterpret_cast<const std::uint8_t *>(before.data());
    const auto *to = reinterpret_cast<const std::uint8_t *>(after.data());
    changes.Compare(RectL{0, 0, 2, 1}, from, 8, to, 8, 4);
    std::vector<std::uint8_t> patch;
    // Masked to 24 bits the changed pixel is 0, so the key is 1.
    CHECK(changes.Patch(to, 8, 4, 0x00FFFFFF, patch) == 1);
    std::uint32_t packed = 0;
    REQUIRE(patch.size() == 4);
    std::memcpy(&packed, patch.data(), 4);
    CHECK(packed == 0xFF000000);
}

} // namespace sfcop::hle::ddraw4
