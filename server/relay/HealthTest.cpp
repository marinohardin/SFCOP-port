#include <doctest/doctest.h>

#include "Health.h"

using namespace sfcop::relay;

TEST_CASE("GET /v1/health answers 200 with the Health object") {
    AdminState s;
    s.bindings = 12;
    s.uptimeSeconds = 3600;
    std::string body = "{\"status\":\"ok\",\"api\":1,\"protocols\":[1],\"sessions\":12,\"uptime_s\":3600}";
    CHECK(handleHttpRequest("GET /v1/health HTTP/1.1\r\nHost: x\r\n\r\n", s) ==
          "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
              std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
}

TEST_CASE("health refuses other methods and unknown paths") {
    AdminState s;
    CHECK(handleHttpRequest("POST /v1/health HTTP/1.1\r\n\r\n", s).find("405") != std::string::npos);
    CHECK(handleHttpRequest("GET /v2/health HTTP/1.1\r\n\r\n", s).find("404") != std::string::npos);
    CHECK(handleHttpRequest("garbage", s).find("400") != std::string::npos);
}

TEST_CASE("DELETE /v1/bindings/<session> queues the removal and answers 204") {
    AdminState s;
    std::string r = handleHttpRequest("DELETE /v1/bindings/5346434f50000001 HTTP/1.1\r\n\r\n", s);
    CHECK(r.find("HTTP/1.1 204 No Content") == 0);
    std::vector<std::uint64_t> q = s.takeRemovals();
    REQUIRE(q.size() == 1);
    CHECK(q[0] == 0x5346434f50000001ull);
    CHECK(s.takeRemovals().empty());
    CHECK(handleHttpRequest("DELETE /v1/bindings/xyz HTTP/1.1\r\n\r\n", s).find("400") !=
          std::string::npos);
    CHECK(handleHttpRequest("GET /v1/bindings/5346434f50000001 HTTP/1.1\r\n\r\n", s).find("405") !=
          std::string::npos);
}
