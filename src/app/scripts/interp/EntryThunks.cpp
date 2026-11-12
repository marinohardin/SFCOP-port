// The pool of entry functions (EntryThunks.h).
#include "app/scripts/interp/EntryThunks.h"

#include "app/scripts/interp/Interpreter.h"

#include <array>
#include <deque>
#include <map>
#include <utility>
#include <vector>

namespace sfcop::scripts::interp {

namespace {

struct Entry {
    Interpreter *interpreter = nullptr;
    std::uint32_t address = 0;
};

std::array<Entry, kEntryThunks> g_entries;

template <std::size_t K> void thunk() {
    const Entry &entry = g_entries[K];
    entry.interpreter->call(entry.address);
}

template <std::size_t... K> constexpr std::array<HostFunction, sizeof...(K)> makeThunks(std::index_sequence<K...>) {
    return {&thunk<K>...};
}

constexpr std::array<HostFunction, kEntryThunks> kThunks = makeThunks(std::make_index_sequence<kEntryThunks>());

std::map<std::pair<const Interpreter *, std::uint32_t>, std::size_t> g_byAddress;
std::deque<std::size_t> g_free; // given back, the longest given back first
std::size_t g_used = 0; // entries ever handed out; those below it and not free are in use

} // namespace

HostFunction entryThunk(Interpreter &interpreter, std::uint32_t address) {
    const auto key = std::make_pair(static_cast<const Interpreter *>(&interpreter), address);
    if (const auto found = g_byAddress.find(key); found != g_byAddress.end()) {
        return kThunks[found->second];
    }
    std::size_t index;
    if (!g_free.empty()) {
        index = g_free.front();
        g_free.pop_front();
    } else if (g_used < kEntryThunks) {
        index = g_used++;
    } else {
        return nullptr;
    }
    g_entries[index] = {&interpreter, address};
    g_byAddress.emplace(key, index);
    return kThunks[index];
}

void releaseEntryThunks(const Interpreter &interpreter, std::uint32_t start, std::uint32_t end) {
    auto it = g_byAddress.lower_bound(std::make_pair(&interpreter, start));
    while (it != g_byAddress.end() && it->first.first == &interpreter && it->first.second < end) {
        g_free.push_back(it->second); // its entry stays, so a kept function fails the call
        it = g_byAddress.erase(it);
    }
}

std::size_t entryThunksInUse() { return g_byAddress.size(); }

} // namespace sfcop::scripts::interp
