#include "hle/ddraw4/ComObject.h"

namespace sfcop::hle::ddraw4 {
namespace {

ComObject::DestroyHook g_destroyHook = nullptr;

} // namespace

void ComObject::SetDestroyHook(DestroyHook hook) {
    g_destroyHook = hook;
}

ComObject::~ComObject() {
    if (g_destroyHook != nullptr) {
        g_destroyHook(this);
    }
}

std::uint32_t ComObject::AddRef() {
    return ++count_;
}

std::uint32_t ComObject::Release() {
    if (count_ == 0) {
        return 0;
    }
    const std::uint32_t remaining = --count_;
    if (remaining == 0) {
        DestroyIfUnused();
    }
    return remaining;
}

void ComObject::ChildGone() {
    --children_;
    DestroyIfUnused();
}

bool ComObject::DestroyIfUnused() {
    if (count_ != 0 || KeepAlive()) {
        return false;
    }
    delete this; // the COM lifetime ends here (see the header)
    return true;
}

} // namespace sfcop::hle::ddraw4
