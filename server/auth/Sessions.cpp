#include "Sessions.h"

#include <vector>

#include "Text.h"

namespace sfcop::auth {

namespace {

// Overwrites the whole allocation of a string that held a password, not only its length.
void wipe(std::string& s) {
    s.assign(s.capacity(), '\0');
    s.clear();
}

// A client-supplied word for a log line: at most 32 bytes, printable ASCII only.
std::string forLog(const std::string& word) {
    std::string out;
    for (char c : word) {
        if (out.size() == 32) break;
        out += (c >= 0x20 && c < 0x7f) ? c : '?';
    }
    return out;
}

}  // namespace

LoginSession::LoginSession(Accounts& accounts, const Logger& log, const Random& random)
    : accounts_(accounts), log_(log), random_(random) {
    for (int i = 0; i < 10; ++i) challenge_ += char('A' + random_() % 26);
    send(buildMessage({{"lc", "1"}, {"challenge", challenge_}, {"id", "1"}}));
}

void LoginSession::fail(int code, const std::string& text) {
    send(buildMessage(
        {{"error", ""}, {"err", std::to_string(code)}, {"fatal", ""}, {"errmsg", text}, {"id", "1"}}));
    close();
}

void LoginSession::newUser(const Message& m) {
    std::string email, nick, password;
    if (!m.get("email", email) || !m.get("nick", nick) || !m.get("password", password) ||
        email.empty() || nick.empty() || password.empty()) {
        log_.info("newuser: incomplete request; refused");
        fail(0x200, "Incomplete request.");
        return;
    }
    const Account& a = accounts_.create(email, nick, password);
    password.assign(password.size(), '\0');
    send(buildMessage({{"nur", ""},
                       {"userid", std::to_string(a.userId)},
                       {"profileid", std::to_string(a.profileId)},
                       {"id", "1"}}));
    log_.info("newuser: account held");
    log_.debug("newuser: " + Accounts::normalEmail(email));
}

void LoginSession::login(const Message& m) {
    std::string clientChallenge, user;
    if (!m.get("challenge", clientChallenge) || !m.get("user", user)) {
        log_.info("login: incomplete request; refused");
        fail(0x100, "Incomplete request.");
        return;
    }
    std::size_t at = user.find('@');
    const Account* a = at == std::string::npos ? nullptr : accounts_.find(user.substr(at + 1));
    if (!a) {
        log_.info("login: no such account");
        if (at != std::string::npos) log_.debug("login: no account for " + user.substr(at + 1));
        fail(kErrorNoAccount, "There is no account for that email; use the new account button.");
        return;
    }
    // The response is not checked: a wrong password fails in the client's check of the proof.
    std::uint32_t sessionKey = random_() % 1000000000u + 1;
    send(buildMessage({{"lc", "2"},
                       {"sesskey", std::to_string(sessionKey)},
                       {"proof", loginProof(a->passwordHash, user, challenge_, clientChallenge)},
                       {"userid", std::to_string(a->userId)},
                       {"profileid", std::to_string(a->profileId)},
                       {"id", "1"}}));
    loggedIn_ = true;
    log_.info("login: answered");
}

void LoginSession::onBytes(const std::uint8_t* data, std::size_t size) {
    if (closing()) return;
    buffer_.append(reinterpret_cast<const char*>(data), size);
    std::vector<Message> messages;
    if (!takeMessages(buffer_, messages)) {
        log_.info("login: malformed message; closed");
        close();
        return;
    }
    for (Message& m : messages) {
        if (closing()) return;
        std::string command = m.command();
        if (command == "login" && !loggedIn_) {
            login(m);
        } else if (command == "newuser" && !loggedIn_) {
            newUser(m);
            // The parsed message and the bytes it came from held the password: wipe both,
            // keeping any bytes of a following message that arrived with it.
            for (auto& pair : m.pairs) wipe(pair.second);
            std::string rest = buffer_;
            wipe(buffer_);
            buffer_ = rest;
        } else if (command == "logout") {
            close();
        }
        // Anything else after the logon (status, profile requests) is ignored.
    }
}

void SearchSession::onBytes(const std::uint8_t* data, std::size_t size) {
    if (closing()) return;
    buffer_.append(reinterpret_cast<const char*>(data), size);
    std::vector<Message> messages;
    if (!takeMessages(buffer_, messages)) {
        close();
        return;
    }
    for (const Message& m : messages) {
        std::string email;
        if (m.command() == "valid" && m.get("email", email)) {
            bool held = accounts_.find(email) != nullptr;
            send(buildMessage({{"vr", held ? "1" : "0"}}));
            log_.info(std::string("valid: ") + (held ? "held" : "not held"));
        } else {
            log_.info("search: " + forLog(m.command()) + " is not served; closed");
        }
        close();
        return;
    }
}

std::string versionCheckResponse(const std::string& head) {
    std::size_t eol = head.find("\r\n");
    std::string line = head.substr(0, eol);
    bool get = line.rfind("GET /motd/vercheck.asp", 0) == 0 &&
               (line.size() == 22 || line[22] == '?' || line[22] == ' ');
    if (!get) {
        return "HTTP/1.0 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    }
    const std::string body = "\\newver\\0";
    return "HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\nContent-Length: " +
           std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
}

void PatchSession::onBytes(const std::uint8_t* data, std::size_t size) {
    if (closing()) return;
    buffer_.append(reinterpret_cast<const char*>(data), size);
    if (buffer_.find("\r\n\r\n") == std::string::npos) {
        if (buffer_.size() > 8192) close();
        return;
    }
    std::string answer = versionCheckResponse(buffer_);
    send(answer);
    log_.info(answer.rfind("HTTP/1.0 200", 0) == 0 ? "vercheck: answered current"
                                                  : "vercheck: not found");
    close();
}

}  // namespace sfcop::auth
