#include "hle/kernel32rt/ThreadLocal.h"

namespace sfcop::hle::kernel32rt {

ThreadLocal::ThreadLocal(platform::Threads &threads) : threads_(threads) {}

Answer ThreadLocal::Allocate() {
    const auto slot = threads_.AllocateTlsSlot();
    if (!slot.Ok()) {
        return Answer{kTlsOutOfIndexes, kErrorNotEnoughMemory};
    }
    return Answer{slot.Value(), std::nullopt};
}

Answer ThreadLocal::Free(std::uint32_t index) {
    const auto freed = threads_.FreeTlsSlot(index);
    return freed.Ok() ? Answer{1, std::nullopt} : Answer{0, kErrorInvalidParameter};
}

Answer ThreadLocal::Get(std::uint32_t index) {
    const auto value = threads_.GetTlsValue(index);
    if (!value.Ok()) {
        return Answer{0, kErrorInvalidParameter};
    }
    return Answer{static_cast<std::uint32_t>(value.Value()), kErrorSuccess};
}

Answer ThreadLocal::Set(std::uint32_t index, std::uint32_t value) {
    const auto set = threads_.SetTlsValue(index, value);
    return set.Ok() ? Answer{1, std::nullopt} : Answer{0, kErrorInvalidParameter};
}

} // namespace sfcop::hle::kernel32rt
