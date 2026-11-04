// The reference count every DirectDraw and Direct3D object of the HLE modules shows the game.
//
// These objects mirror COM lifetimes, so they are the one place in src/ that uses raw new and
// `delete this` (CONVENTIONS §4.2): the game, not C++ scope, decides when one dies. An object is
// created with a count of 1 (the reference its creator hands out). AddRef() and Release() return
// the new count, as IUnknown's do. When the count reaches 0 the object is destroyed, unless
// KeepAlive() says something inside the module still needs it (a DirectDraw object with surfaces
// left, the back buffer of a flipping chain); such an object is destroyed later by whatever kept
// it.
//
// Every interface of one object (IDirectDraw, IDirectDraw2 and IDirectDraw4; IDirectDrawSurface3
// and IDirectDrawSurface4) shares this one count: QueryInterface for another interface adds a
// reference here, and a Release through any interface takes one away. The game only ever balances
// its own AddRef and Release calls, so the totals it sees are the same as with a count per
// interface.
//
// The guest layer keeps the game's objects (vtable pointers in guest memory) for each host object.
// It learns of a destruction through the hook, so that it can free them.
#pragma once

#include <cstdint>

namespace sfcop::hle::ddraw4 {

class ComObject {
public:
    // Called from the destructor of every ComObject, with the object being destroyed. Its
    // derived parts are gone by then; the pointer serves as a key only.
    using DestroyHook = void (*)(const ComObject *object);

    // Installs the hook for every object of the process (nullptr removes it).
    static void SetDestroyHook(DestroyHook hook);

    ComObject(const ComObject &) = delete;
    ComObject &operator=(const ComObject &) = delete;

    // Adds a reference; returns the new count.
    std::uint32_t AddRef();

    // Removes a reference and returns the new count. At 0 the object is destroyed, unless
    // KeepAlive() holds it. A Release at 0 does nothing and returns 0.
    std::uint32_t Release();

    std::uint32_t RefCount() const { return count_; }

    // An object made by this one (a surface of a DirectDraw object, a device of a Direct3D
    // object) keeps it alive without a reference the game can see: ChildGone() at its
    // destruction may destroy this object.
    void AdoptChild() { ++children_; }
    void ChildGone();

protected:
    explicit ComObject(std::uint32_t initialCount = 1) : count_(initialCount) {}
    virtual ~ComObject();

    // True while the module itself still needs the object after the game's last Release: by
    // default, while objects it made are alive.
    virtual bool KeepAlive() const { return children_ != 0; }

    // Destroys the object if the game holds no reference and KeepAlive() no longer holds it.
    // Returns true if it was destroyed; `this` must not be used afterwards.
    bool DestroyIfUnused();

private:
    std::uint32_t count_;
    std::uint32_t children_ = 0;
};

} // namespace sfcop::hle::ddraw4
