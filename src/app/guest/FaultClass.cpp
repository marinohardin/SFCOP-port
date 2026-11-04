#include "FaultClass.h"

#include <algorithm>
#include <cstring>

namespace sfcop::guest {

CodeRange SpanOfFunctions(const std::uintptr_t *addresses, std::size_t count,
                          std::uintptr_t margin) {
    if (count == 0) {
        return {};
    }
    const auto [low, high] = std::minmax_element(addresses, addresses + count);
    return CodeRange{*low, *high + margin};
}

bool IsLeafMemoryRoutine(const char *symbol) {
    if (symbol == nullptr) {
        return false;
    }
    for (const char *name : {"memmove", "memcpy", "memset", "memcmp", "bzero"}) {
        if (std::strstr(symbol, name) != nullptr) {
            return true;
        }
    }
    return false;
}

} // namespace sfcop::guest
