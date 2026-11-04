// Typed handles for the objects a component owns (textures, voices, sockets, files, ...). A
// handle is an opaque number chosen by the backend; zero is never a live handle. The tag type
// keeps a TextureId from being passed where a VoiceId is expected.
#pragma once

#include <cstdint>

namespace sfcop::platform {

template <typename Tag> struct Handle {
    using TagType = Tag;

    std::uint32_t value = 0;

    constexpr bool Valid() const { return value != 0; }
    friend constexpr bool operator==(Handle a, Handle b) { return a.value == b.value; }
    friend constexpr bool operator!=(Handle a, Handle b) { return a.value != b.value; }
};

} // namespace sfcop::platform
