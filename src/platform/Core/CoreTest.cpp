#include "platform/Core/Result.h"
#include "platform/Core/Handle.h"
#include "platform/Core/MockLog.h"

#include <doctest/doctest.h>

#include <string>
#include <vector>

namespace sfcop::platform {
namespace {

struct TestTag {};
using TestId = Handle<TestTag>;

enum class Colour { kRed = 3 };

struct Opaque {
    int field;
};

Result<int> parseDigit(char c) {
    if (c < '0' || c > '9') {
        return Error::kInvalidArgument;
    }
    return c - '0';
}

} // namespace

TEST_CASE("a successful result carries its value and no error") {
    Result<int> result = parseDigit('7');
    CHECK(result.Ok());
    CHECK(static_cast<bool>(result));
    CHECK(result.Value() == 7);
    CHECK(result.Code() == Error::kNone);
    CHECK(result.ValueOr(-1) == 7);
}

TEST_CASE("a failed result carries its error and falls back to the default") {
    Result<int> result = parseDigit('x');
    CHECK_FALSE(result.Ok());
    CHECK(result.Code() == Error::kInvalidArgument);
    CHECK(result.ValueOr(-1) == -1);
}

TEST_CASE("a default status is success and a status built from an error fails") {
    Status success;
    CHECK(success.Ok());
    CHECK(success.Code() == Error::kNone);
    Status failure = Error::kUnsupported;
    CHECK_FALSE(failure.Ok());
    CHECK(failure.Code() == Error::kUnsupported);
}

TEST_CASE("every error has a readable name") {
    CHECK(std::string(errorName(Error::kUnsupported)) == "Unsupported");
    CHECK(std::string(errorName(Error::kWouldBlock)) == "WouldBlock");
    CHECK(std::string(errorName(Error::kHostUnreachable)) == "HostUnreachable");
}

TEST_CASE("handle zero is never valid and handles compare by value") {
    TestId none;
    CHECK_FALSE(none.Valid());
    TestId a{5};
    TestId b{5};
    TestId c{6};
    CHECK(a.Valid());
    CHECK(a == b);
    CHECK(a != c);
}

TEST_CASE("the mock log writes each call as one line with its arguments") {
    MockLog log;
    log.Record("Open");
    log.Record("Seek", TestId{4}, -12, 7u, true, Colour::kRed);
    log.Record("Name", std::string("TestCaptain"), "Settings/a.gf");
    log.Record("Gain", 0.5f, static_cast<unsigned char>(200), Opaque{1});
    CHECK(log.Calls() == std::vector<std::string>{
                             "Open()",
                             "Seek(4, -12, 7, true, 3)",
                             "Name(\"TestCaptain\", \"Settings/a.gf\")",
                             "Gain(0.5, 200, {})",
                         });
    CHECK(log.Count("Seek") == 1);
    CHECK(log.Count("See") == 0);
    log.Clear();
    CHECK(log.Calls().empty());
}

} // namespace sfcop::platform
