// The auth service's three sessions, one per TCP connection (src/net/DIRECTORY_API.md):
//  - LoginSession: the account login (TCP 29900); the service speaks first with a challenge.
//  - SearchSession: the profile search's email check (TCP 29901).
//  - PatchSession: the version check over HTTP (TCP 80), always "current".
// They take bytes and give bytes; the TcpServer (or a test) carries them. None of them logs a
// password, a hash, a proof, a challenge or a session key; an email appears only at debug.
#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "Accounts.h"
#include "Log.h"
#include "TcpServer.h"
#include "Text.h"

namespace sfcop::auth {

using Random = std::function<std::uint32_t()>;

// Error code for "no such account": the client has special cases only for 0x106 and 0x201
// (STARFLEETOP 0x006BB1D0), so any other code is a plain failed logon.
constexpr int kErrorNoAccount = 0x103;

class LoginSession : public StreamSession {
public:
    LoginSession(Accounts& accounts, const Logger& log, const Random& random);
    void onBytes(const std::uint8_t* data, std::size_t size) override;
    const std::string& challenge() const { return challenge_; }

private:
    void login(const Message& m);
    void newUser(const Message& m);
    void fail(int code, const std::string& text);

    Accounts& accounts_;
    const Logger& log_;
    const Random& random_;
    std::string challenge_;
    std::string buffer_;
    bool loggedIn_ = false;
};

class SearchSession : public StreamSession {
public:
    SearchSession(const Accounts& accounts, const Logger& log) : accounts_(accounts), log_(log) {}
    void onBytes(const std::uint8_t* data, std::size_t size) override;

private:
    const Accounts& accounts_;
    const Logger& log_;
    std::string buffer_;
};

class PatchSession : public StreamSession {
public:
    explicit PatchSession(const Logger& log) : log_(log) {}
    void onBytes(const std::uint8_t* data, std::size_t size) override;

private:
    const Logger& log_;
    std::string buffer_;
};

// The version check's answer for a raw request head; a pure function for the tests.
std::string versionCheckResponse(const std::string& requestHead);

}  // namespace sfcop::auth
