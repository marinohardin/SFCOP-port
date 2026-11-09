#include "app/ServiceHosts.h"

#include <cctype>
#include <fstream>
#include <sstream>

namespace sfcop::app {
namespace {

// The smallest JSON reader servers.json needs: it walks one value and keeps the string members
// of the top-level "services" object; everything else is checked for form and skipped.
class Reader {
public:
    explicit Reader(const std::string &text) : text_(text) {}

    bool topLevel(ServiceHosts &out) {
        skipSpace();
        if (!object(&out, 0)) {
            return false;
        }
        skipSpace();
        return pos_ == text_.size();
    }

private:
    const std::string &text_;
    std::size_t pos_ = 0;

    void skipSpace() {
        while (pos_ < text_.size() && std::isspace(static_cast<unsigned char>(text_[pos_]))) {
            ++pos_;
        }
    }
    bool take(char c) {
        skipSpace();
        if (pos_ < text_.size() && text_[pos_] == c) {
            ++pos_;
            return true;
        }
        return false;
    }
    bool string(std::string &out) {
        if (!take('"')) {
            return false;
        }
        out.clear();
        while (pos_ < text_.size()) {
            char c = text_[pos_++];
            if (c == '"') {
                return true;
            }
            if (c == '\\') {
                if (pos_ >= text_.size()) {
                    return false;
                }
                char e = text_[pos_++];
                switch (e) {
                case '"': case '\\': case '/': out += e; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u':
                    // Host names are ASCII; an escaped code point is kept only if it is one.
                    if (pos_ + 4 > text_.size()) {
                        return false;
                    }
                    {
                        unsigned v = 0;
                        for (int i = 0; i < 4; ++i) {
                            char h = text_[pos_++];
                            if (!std::isxdigit(static_cast<unsigned char>(h))) {
                                return false;
                            }
                            v = v * 16 + unsigned(std::isdigit(static_cast<unsigned char>(h))
                                                      ? h - '0'
                                                      : (std::tolower(h) - 'a' + 10));
                        }
                        out += v < 0x80 ? static_cast<char>(v) : '?';
                    }
                    break;
                default: return false;
                }
            } else if (static_cast<unsigned char>(c) < 0x20) {
                return false;
            } else {
                out += c;
            }
        }
        return false;
    }
    bool literal(const char *word) {
        skipSpace();
        std::size_t n = std::char_traits<char>::length(word);
        if (text_.compare(pos_, n, word) != 0) {
            return false;
        }
        pos_ += n;
        return true;
    }
    bool number() {
        skipSpace();
        std::size_t start = pos_;
        while (pos_ < text_.size() &&
               (std::isdigit(static_cast<unsigned char>(text_[pos_])) || text_[pos_] == '-' ||
                text_[pos_] == '+' || text_[pos_] == '.' || text_[pos_] == 'e' ||
                text_[pos_] == 'E')) {
            ++pos_;
        }
        return pos_ > start;
    }
    // `services` is the object to fill when this value is the "services" member of the top level.
    bool value(ServiceHosts *services, int depth, std::string *asString) {
        if (depth > 16) {
            return false;
        }
        skipSpace();
        if (pos_ >= text_.size()) {
            return false;
        }
        char c = text_[pos_];
        if (c == '{') {
            return object(services, services ? 1 : depth + 1);
        }
        if (c == '[') {
            ++pos_;
            if (take(']')) {
                return true;
            }
            do {
                if (!value(nullptr, depth + 1, nullptr)) {
                    return false;
                }
            } while (take(','));
            return take(']');
        }
        if (c == '"') {
            std::string s;
            if (!string(s)) {
                return false;
            }
            if (asString) {
                *asString = s;
            }
            return true;
        }
        return literal("true") || literal("false") || literal("null") || number();
    }
    // At depth 0 `out` receives the services; at depth 1 `out` is the services object itself.
    bool object(ServiceHosts *out, int depth) {
        if (!take('{')) {
            return false;
        }
        if (take('}')) {
            return true;
        }
        do {
            std::string key;
            skipSpace();
            if (!string(key) || !take(':')) {
                return false;
            }
            if (depth == 0 && key == "services" && out) {
                if (!value(out, 1, nullptr)) {
                    return false;
                }
                continue;
            }
            std::string text;
            if (!value(nullptr, depth + 1, depth == 1 && out ? &text : nullptr)) {
                return false;
            }
            if (depth == 1 && out && !text.empty()) {
                if (key == "directory") out->directory = text;
                else if (key == "auth") out->auth = text;
                else if (key == "cdkey") out->cdkey = text;
                else if (key == "patch") out->patch = text;
            }
        } while (take(','));
        return take('}');
    }
};

} // namespace

bool parseServiceHosts(const std::string &json, ServiceHosts &out, std::string &error) {
    ServiceHosts read;
    Reader reader(json);
    if (!reader.topLevel(read)) {
        error = "not a JSON object";
        return false;
    }
    out = read;
    return true;
}

std::map<std::string, std::string> serviceAliases(const ServiceHosts &hosts) {
    // The host names the original carries (src/net/DIRECTORY_API.md, "Pointing the client at the
    // stand-ins"), by where the exe reads each one.
    return {
        // The directory: ServerSetup/WONDirectoryServer/Addresses in the server setup file,
        // read by STARFLEETOP 0x005D4EED.
        {"sfc2.west.won.net", hosts.directory},
        {"sfc2.central.won.net", hosts.directory},
        {"sfc2.east.won.net", hosts.directory},
        // The account login service, STARFLEETOP 0x006BAB30, and its profile search,
        // STARFLEETOP 0x006BB930.
        {"gpcm.gamespy.com", hosts.auth},
        {"gpsp.gamespy.com", hosts.auth},
        // The CD-key service, STARFLEETOP 0x006C1960.
        {"key.gamespy.com", hosts.cdkey},
        // The version check's URL, STARFLEETOP 0x006C99B0.
        {"motd.gamespy.com", hosts.patch},
        // The server list service, STARFLEETOP 0x006CB690 and 0x006CCB20: pointed at the
        // directory's host so that the client reaches no one else's server.
        {"master.gamespy.com", hosts.directory},
    };
}

ServiceHosts loadServiceHosts(const std::string &dataDir, std::string &warning) {
    ServiceHosts hosts;
    std::ifstream in(dataDir + "/servers.json", std::ios::binary);
    if (!in) {
        return hosts;
    }
    std::ostringstream text;
    text << in.rdbuf();
    std::string error;
    if (!parseServiceHosts(text.str(), hosts, error)) {
        warning = "servers.json is not valid JSON; no service is reachable until servers.json is fixed";
    }
    return hosts;
}

} // namespace sfcop::app
