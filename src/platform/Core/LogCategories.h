// Which optional kinds of log output are on. Everything the client prints goes to stderr (and, in
// the app bundle, to the run's log file); some of it is for a developer chasing one problem and
// buries what a player or the owner needs after a crash. Those kinds are categories, off by
// default and switched on by name with `--log LIST` or the environment variable SFCOP_LOG, a comma
// separated list; `all` switches every one on. A name nothing knows is ignored.
//
// Categories:
//   guest-debug  the original game's own debug text (what it passes to OutputDebugString), whole
//   override     the report of which decompiled functions run in place of lifted ones, and the
//                exit summary of their call counts
//
// The C entry point is for the guest-side files, which are C or compile against the lift.
#pragma once

#ifdef __cplusplus
#include <string_view>

namespace sfcop::platform {

// True when `category` is named in `list` (a comma separated list, spaces around names ignored)
// or `list` names `all`.
bool LogCategoryInList(std::string_view list, std::string_view category);

// Adds `list` to the categories SFCOP_LOG names for this process (what `--log` does).
void AddLogCategories(std::string_view list);

} // namespace sfcop::platform

extern "C" {
#endif

// True when `category` is on in the environment's SFCOP_LOG.
int sfcop_log_enabled(const char *category);

#ifdef __cplusplus
}
#endif
