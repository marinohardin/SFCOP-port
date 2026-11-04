#include "hle/kernel32rt/Modules.h"

#include <cctype>

namespace sfcop::hle::kernel32rt {

std::string Modules::Normalise(std::string_view name) {
    const auto slash = name.find_last_of("\\/:");
    if (slash != std::string_view::npos) {
        name.remove_prefix(slash + 1);
    }
    std::string normal;
    normal.reserve(name.size() + 4);
    for (char c : name) {
        normal.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    if (normal.find('.') == std::string::npos) {
        normal += ".dll";
    } else if (!normal.empty() && normal.back() == '.') {
        normal.pop_back(); // "name." means the name with no extension
    }
    return normal;
}

void Modules::Add(std::string_view name, std::uint32_t handle) {
    std::lock_guard<std::mutex> hold(lock_);
    handles_[Normalise(name)] = handle;
    exports_[handle];
}

void Modules::AddExport(std::uint32_t handle, std::string_view function, std::uint32_t address) {
    std::lock_guard<std::mutex> hold(lock_);
    exports_[handle][std::string(function)] = address;
}

std::optional<std::uint32_t> Modules::Find(std::string_view name) const {
    std::lock_guard<std::mutex> hold(lock_);
    const auto found = handles_.find(Normalise(name));
    if (found == handles_.end()) {
        return std::nullopt;
    }
    return found->second;
}

bool Modules::IsBuiltIn(std::uint32_t handle) const {
    std::lock_guard<std::mutex> hold(lock_);
    return exports_.count(handle) != 0;
}

std::optional<std::uint32_t> Modules::Export(std::uint32_t handle,
                                             std::string_view function) const {
    std::lock_guard<std::mutex> hold(lock_);
    const auto module = exports_.find(handle);
    if (module == exports_.end()) {
        return std::nullopt;
    }
    const auto found = module->second.find(function);
    if (found == module->second.end()) {
        return std::nullopt;
    }
    return found->second;
}

} // namespace sfcop::hle::kernel32rt
