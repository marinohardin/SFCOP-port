#include "Accounts.h"

#include "Md5.h"

namespace sfcop::auth {

std::string Accounts::normalEmail(const std::string& email) {
    std::string out = email;
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    }
    return out;
}

const Account& Accounts::create(const std::string& email, const std::string& nick,
                                const std::string& password) {
    Account& a = table_[normalEmail(email)];
    a.passwordHash = md5Hex(password);
    a.nick = nick;
    if (a.userId == 0) {  // a replaced account keeps its ids
        a.userId = nextId_;
        a.profileId = nextId_;
        ++nextId_;
    }
    return a;
}

const Account* Accounts::find(const std::string& email) const {
    auto it = table_.find(normalEmail(email));
    return it == table_.end() ? nullptr : &it->second;
}

std::string loginProof(const std::string& passwordHash, const std::string& user,
                       const std::string& serverChallenge, const std::string& clientChallenge) {
    return md5Hex(passwordHash + std::string(48, ' ') + user + serverChallenge + clientChallenge +
                  passwordHash);
}

}  // namespace sfcop::auth
