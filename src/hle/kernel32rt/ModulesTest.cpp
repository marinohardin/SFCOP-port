#include "hle/kernel32rt/Modules.h"

#include <doctest/doctest.h>

namespace sfcop::hle::kernel32rt {
namespace {

TEST_CASE("a module name compares as Windows compares it") {
    CHECK(Modules::Normalise("OSSpecific98") == "osspecific98.dll");
    CHECK(Modules::Normalise("OSSPECIFIC98.DLL") == "osspecific98.dll");
    CHECK(Modules::Normalise("C:\\Game\\OSSpecific95.dll") == "osspecific95.dll");
    CHECK(Modules::Normalise("plugin.") == "plugin");
    CHECK(Modules::Normalise("mss32.dll") == "mss32.dll");
}

TEST_CASE("LoadLibraryA by a bare name finds a built-in module and its export") {
    Modules modules;
    modules.Add("OSSpecific98.dll", 0x7FF00000u);
    modules.AddExport(0x7FF00000u, "OS_InterlockedCompareExchange", 0x7E000040u);
    CHECK(modules.Find("OSSpecific98") == 0x7FF00000u);
    CHECK(modules.IsBuiltIn(0x7FF00000u));
    CHECK(modules.Export(0x7FF00000u, "OS_InterlockedCompareExchange") == 0x7E000040u);
}

TEST_CASE("a name or export that is not built in is not found") {
    Modules modules;
    modules.Add("OSSpecific98.dll", 0x7FF00000u);
    CHECK_FALSE(modules.Find("kernel32"));
    CHECK_FALSE(modules.IsBuiltIn(0x00400000u));
    CHECK_FALSE(modules.Export(0x7FF00000u, "Missing"));
    CHECK_FALSE(modules.Export(0x00400000u, "OS_InterlockedCompareExchange"));
}

} // namespace
} // namespace sfcop::hle::kernel32rt
