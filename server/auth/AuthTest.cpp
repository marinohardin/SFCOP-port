// The account login, the email check and the version check on hand-written bytes. Expected digests
// were computed once outside the code, with Python's hashlib (src/net/DIRECTORY_API.md, "Worked
// example"); every value is invented.
#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "Accounts.h"
#include "Md5.h"
#include "Sessions.h"
#include "Text.h"

using namespace sfcop::auth;

namespace {

struct Captured {
    std::vector<std::string> lines;
    Logger logger{LogLevel::kDebug,
                  [this](LogLevel, const std::string& s) { lines.push_back(s); }};
    // True if any line holds `text`.
    bool saw(const std::string& text) const {
        for (const std::string& l : lines) {
            if (l.find(text) != std::string::npos) return true;
        }
        return false;
    }
};

// 0, 1, 2, ...: the challenge is then ABCDEFGHIJ and the session key 11.
struct Counter {
    std::uint32_t next = 0;
    Random random{[this] { return next++; }};
};

void feed(StreamSession& s, const std::string& text) {
    s.onBytes(reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
}

const char* const kH = "8957a7e865c41c02e58f18bcd7c9f2b9";  // md5hex("throwaway")
const char* const kUser = "TestCaptain@testcaptain@example.test";
const char* const kClientChallenge = "abcdefghijklmnopqrstuvwxyz012345";
const char* const kProof = "3c3391c2ffbea131d0a5e220b76ab18f";
const char* const kWrongProof = "21012fb266f527997d41f7e21946d197";  // from the password "wrong"

const std::string kLogin = std::string("\\login\\\\challenge\\") + kClientChallenge + "\\user\\" +
                           kUser + "\\response\\ff6ea2a727e6b6599d9593a189db652a\\port\\6500" +
                           "\\id\\1\\final\\";
const std::string kNewUser =
    "\\newuser\\\\email\\TestCaptain@Example.test\\nick\\TestCaptain\\password\\throwaway"
    "\\id\\1\\final\\";

}  // namespace

TEST_CASE("MD5 gives the published RFC 1321 digests") {
    CHECK(md5Hex("") == "d41d8cd98f00b204e9800998ecf8427e");
    CHECK(md5Hex("abc") == "900150983cd24fb0d6963f7d28e17f72");
    CHECK(md5Hex("message digest") == "f96b697d7cb7938d525a2f31aaf161d0");
    CHECK(md5Hex("12345678901234567890123456789012345678901234567890123456789012345678901234567890") ==
          "57edf4a22be3c955ac49da2e2107b67a");
}

TEST_CASE("the proof from hand-written challenges, user and H is the worked value") {
    CHECK(md5Hex("throwaway") == kH);
    CHECK(loginProof(kH, kUser, "ABCDEFGHIJ", kClientChallenge) == kProof);
}

TEST_CASE("text messages split on \\final\\ and keep empty values") {
    std::string buffer = kLogin + "\\logout\\\\sesskey\\11\\final\\\\partial";
    std::vector<Message> out;
    REQUIRE(takeMessages(buffer, out));
    REQUIRE(out.size() == 2);
    CHECK(out[0].command() == "login");
    std::string v;
    REQUIRE(out[0].get("user", v));
    CHECK(v == kUser);
    CHECK(out[1].command() == "logout");
    CHECK(buffer == "\\partial");
    CHECK(buildMessage({{"vr", "1"}}) == "\\vr\\1\\final\\");
    CHECK(buildMessage({{"error", ""}, {"errmsg", "a\\b"}}) == "\\error\\\\errmsg\\ab\\final\\");

    std::string junk = "no backslash\\final\\";
    CHECK_FALSE(takeMessages(junk, out));
    std::string endless(5000, 'x');
    CHECK_FALSE(takeMessages(endless, out));
}

TEST_CASE("the login service speaks first with a ten-letter challenge") {
    Accounts accounts;
    Captured log;
    Counter counter;
    LoginSession s(accounts, log.logger, counter.random);
    CHECK(s.takeOutput() == "\\lc\\1\\challenge\\ABCDEFGHIJ\\id\\1\\final\\");
}

TEST_CASE("new account, then logon: the reply carries the proof the client expects") {
    Accounts accounts;
    Captured log;
    Counter counter;
    LoginSession s(accounts, log.logger, counter.random);
    s.takeOutput();
    feed(s, kNewUser);
    CHECK(s.takeOutput() == "\\nur\\\\userid\\1\\profileid\\1\\id\\1\\final\\");
    CHECK(accounts.size() == 1);
    feed(s, kLogin);
    CHECK(s.takeOutput() == std::string("\\lc\\2\\sesskey\\11\\proof\\") + kProof +
                                "\\userid\\1\\profileid\\1\\id\\1\\final\\");
    CHECK_FALSE(s.closing());
    // later messages are ignored; a logout ends the connection
    feed(s, "\\status\\1\\sesskey\\11\\final\\");
    CHECK(s.takeOutput().empty());
    feed(s, "\\logout\\\\sesskey\\11\\final\\");
    CHECK(s.closing());
}

TEST_CASE("a logon with a different password yields a proof the client rejects") {
    Accounts accounts;
    accounts.create("testcaptain@example.test", "TestCaptain", "wrong");
    Captured log;
    Counter counter;
    LoginSession s(accounts, log.logger, counter.random);
    s.takeOutput();
    feed(s, kLogin);  // the client typed "throwaway"; its own check expects kProof
    std::string reply = s.takeOutput();
    CHECK(reply.find(std::string("\\proof\\") + kWrongProof + "\\") != std::string::npos);
    CHECK(reply.find(kProof) == std::string::npos);
}

TEST_CASE("a logon for an email the service does not hold gets the no-account error") {
    Accounts accounts;
    Captured log;
    Counter counter;
    LoginSession s(accounts, log.logger, counter.random);
    s.takeOutput();
    feed(s, kLogin);
    CHECK(s.takeOutput() ==
          "\\error\\\\err\\259\\fatal\\\\errmsg\\There is no account for that email; use the new "
          "account button.\\id\\1\\final\\");
    CHECK(s.closing());
}

TEST_CASE("a restart empties the table: the same logon then fails") {
    Captured log;
    Counter counter;
    {
        Accounts before;
        before.create("testcaptain@example.test", "TestCaptain", "throwaway");
        REQUIRE(before.find("TESTCAPTAIN@example.test") != nullptr);
    }
    Accounts afterRestart;  // a new process starts with a new table
    CHECK(afterRestart.size() == 0);
    CHECK(afterRestart.find("testcaptain@example.test") == nullptr);
    LoginSession s(afterRestart, log.logger, counter.random);
    s.takeOutput();
    feed(s, kLogin);
    CHECK(s.takeOutput().rfind("\\error\\\\err\\259\\", 0) == 0);
}

TEST_CASE("a second new-account request for an email replaces the first and keeps its ids") {
    Accounts accounts;
    const Account& first = accounts.create("testcaptain@example.test", "TestCaptain", "one");
    std::uint32_t id = first.userId;
    const Account& second = accounts.create("TestCaptain@Example.TEST", "Captain2", "throwaway");
    CHECK(accounts.size() == 1);
    CHECK(second.userId == id);
    CHECK(second.nick == "Captain2");
    CHECK(second.passwordHash == kH);
}

TEST_CASE("an incomplete new-account or logon request is refused") {
    Accounts accounts;
    Captured log;
    Counter counter;
    LoginSession a(accounts, log.logger, counter.random);
    a.takeOutput();
    feed(a, "\\newuser\\\\email\\x@example.test\\nick\\X\\id\\1\\final\\");
    CHECK(a.takeOutput().rfind("\\error\\\\err\\512\\", 0) == 0);
    CHECK(accounts.size() == 0);
    LoginSession b(accounts, log.logger, counter.random);
    b.takeOutput();
    feed(b, "\\login\\\\user\\X@x@example.test\\final\\");
    CHECK(b.takeOutput().rfind("\\error\\\\err\\256\\", 0) == 0);
}

TEST_CASE("no password, hash, proof, challenge or session key is logged, even at debug") {
    Accounts accounts;
    Captured log;
    Counter counter;
    LoginSession s(accounts, log.logger, counter.random);
    feed(s, kNewUser);
    feed(s, kLogin);
    LoginSession t(accounts, log.logger, counter.random);
    feed(t, "\\login\\\\challenge\\x\\user\\X@nobody@example.test\\final\\");
    REQUIRE_FALSE(log.lines.empty());
    for (const char* secret : {"throwaway", kH, kProof, "ABCDEFGHIJ", kClientChallenge,
                               "sesskey", "ff6ea2a727e6b6599d9593a189db652a"}) {
        CHECK_FALSE(log.saw(secret));
    }
    CHECK(log.saw("testcaptain@example.test"));  // the email, at debug only
}

TEST_CASE("the email check answers 1 only for an email the service holds") {
    Accounts accounts;
    accounts.create("testcaptain@example.test", "TestCaptain", "throwaway");
    Captured log;
    SearchSession held(accounts, log.logger);
    feed(held, "\\valid\\\\email\\TestCaptain@example.test\\final\\");
    CHECK(held.takeOutput() == "\\vr\\1\\final\\");
    CHECK(held.closing());
    SearchSession other(accounts, log.logger);
    feed(other, "\\valid\\\\email\\other@example.test\\final\\");
    CHECK(other.takeOutput() == "\\vr\\0\\final\\");
    SearchSession unknown(accounts, log.logger);
    feed(unknown, "\\search\\\\nick\\x\\final\\");
    CHECK(unknown.takeOutput().empty());
    CHECK(unknown.closing());
}

TEST_CASE("a command word the service does not serve is logged at 32 printable bytes at most") {
    Accounts accounts;
    Captured log;
    SearchSession s(accounts, log.logger);
    // Forty bytes, with a control byte inside the first thirty-two.
    feed(s, "\\" + std::string(5, 'a') + "\x01" + std::string(34, 'b') + "\\\\final\\");
    CHECK(log.saw("search: aaaaa?" + std::string(26, 'b') + " is not served; closed"));
    CHECK_FALSE(log.saw(std::string(27, 'b')));
}

TEST_CASE("the version check answers current for its path and 404 otherwise") {
    Captured log;
    PatchSession s(log.logger);
    feed(s, "GET /motd/vercheck.asp?productid=808&versionuniqueid=2.5.5.2&distid=270 HTTP/1.1\r\n");
    CHECK(s.takeOutput().empty());  // waits for the end of the head
    feed(s, "Host: patch.example.test\r\n\r\n");
    CHECK(s.takeOutput() == "HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 9\r\n"
                            "Connection: close\r\n\r\n\\newver\\0");
    CHECK(s.closing());
    CHECK(versionCheckResponse("GET /motd/other.asp HTTP/1.1\r\n\r\n").rfind("HTTP/1.0 404", 0) == 0);
    CHECK(versionCheckResponse("POST /motd/vercheck.asp HTTP/1.1\r\n\r\n").rfind("HTTP/1.0 404", 0) ==
          0);
    CHECK(versionCheckResponse("GET /motd/vercheck.aspx HTTP/1.1\r\n\r\n").rfind("HTTP/1.0 404", 0) ==
          0);
}
