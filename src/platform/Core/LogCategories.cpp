#include "platform/Core/LogCategories.h"

#include <cstdlib>
#include <string>

namespace sfcop::platform {

namespace {

std::string_view trimmed(std::string_view text) {
    const char *space = " \t\r\n";
    const std::size_t first = text.find_first_not_of(space);
    if (first == std::string_view::npos) {
        return {};
    }
    return text.substr(first, text.find_last_not_of(space) - first + 1);
}

} // namespace

bool LogCategoryInList(std::string_view list, std::string_view category) {
    while (!list.empty()) {
        const std::size_t comma = list.find(',');
        const std::string_view name = trimmed(list.substr(0, comma));
        if (!name.empty() && (name == category || name == "all")) {
            return true;
        }
        if (comma == std::string_view::npos) {
            break;
        }
        list.remove_prefix(comma + 1);
    }
    return false;
}

void AddLogCategories(std::string_view list) {
    std::string joined(list);
    if (const char *existing = std::getenv("SFCOP_LOG"); existing != nullptr && *existing != '\0') {
        joined = std::string(existing) + "," + joined;
    }
#ifdef _WIN32
    _putenv_s("SFCOP_LOG", joined.c_str());
#else
    setenv("SFCOP_LOG", joined.c_str(), 1);
#endif
}

} // namespace sfcop::platform

extern "C" int sfcop_log_enabled(const char *category) {
    const char *list = std::getenv("SFCOP_LOG");
    return list != nullptr && category != nullptr &&
           sfcop::platform::LogCategoryInList(list, category);
}
