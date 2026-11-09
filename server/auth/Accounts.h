// The stand-in's accounts, in process memory only (src/net/DIRECTORY_API.md, "The stand-in's
// accounts"; an interim answer under DESIGN §12.3, not a decision). Per lower-cased email it keeps
// the password's hash H as the client computes it, the nick, and the two ids it assigned. Nothing
// is written anywhere; a new table is a restart.
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>

namespace sfcop::auth {

struct Account {
    std::string passwordHash;  // H = md5hex(password); never logged, never sent
    std::string nick;
    std::uint32_t userId = 0;
    std::uint32_t profileId = 0;
};

class Accounts {
public:
    // Creates the account, or replaces the one held for that email. The password is used only
    // to compute H here and is not kept. Returns the account as held.
    const Account& create(const std::string& email, const std::string& nick,
                          const std::string& password);
    // Null when the table holds no account for that email (compared lower-cased).
    const Account* find(const std::string& email) const;
    std::size_t size() const { return table_.size(); }

    static std::string normalEmail(const std::string& email);

private:
    std::map<std::string, Account> table_;
    std::uint32_t nextId_ = 1;
};

// The proof the client expects after a logon: md5hex(H + 48 spaces + user + server challenge +
// client challenge + H), with `user` the `<nick>@<email>` the client sent.
std::string loginProof(const std::string& passwordHash, const std::string& user,
                       const std::string& serverChallenge, const std::string& clientChallenge);

}  // namespace sfcop::auth
