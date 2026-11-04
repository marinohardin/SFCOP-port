// fixes.h as VC6 sees it: every build switch is off, and only the preprocessor part is read.
// Where _MSC_VER is already defined (MSVC, clang-cl) SFCOP_FIXES_OFF stands in for it.
#include <doctest/doctest.h>

#if defined(_MSC_VER)
#define SFCOP_FIXES_OFF
#include "fixes/fixes.h"
#else
#define SFCOP_FIXES_TEST_VC6
#define _MSC_VER 1200
#include "fixes/fixes.h"
#undef _MSC_VER
#endif

// A build switch declared default on, as fixes.h asks.
#define SFCOP_FIX_BUILD_fix_test_vc6 1
#define SFCOP_FIX_fix_test_vc6() SFCOP_FIX_POSITION(SFCOP_FIX_BUILD_fix_test_vc6)

namespace {

#if SFCOP_FIX(fix_test_vc6)
constexpr int kVc6Region = 1;  // the fix
#else
constexpr int kVc6Region = 0;  // the original
#endif

}  // namespace

#if defined(SFCOP_FIXES_TEST_VC6)
// Compiles only if fixes.h declared no sfcop::fixes::registry: VC6 reads no C++17.
namespace sfcop::fixes {
int registry = 1;
}
#endif

TEST_CASE("under VC6 a build switch declared on compiles the original") {
    CHECK(kVc6Region == 0);
    CHECK(SFCOP_FIX(fix_test_vc6) == 0);
#if defined(SFCOP_FIXES_TEST_VC6)
    CHECK(sfcop::fixes::registry == 1);
#endif
}
