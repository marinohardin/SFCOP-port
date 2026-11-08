// The doctest runner for sfcop_lobby_tests. The service logs to stdout by default; the tests are
// silent unless one of them installs a capturing sink.
#include "Log.h"

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

namespace {
const bool silenced = [] {
    sfcop::lobby::log::setSink([](sfcop::lobby::LogLevel, const std::string&) {});
    return true;
}();
} // namespace
