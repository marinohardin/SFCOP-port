#include "Http.h"

#include <cctype>
#include <cstdlib>

namespace sfcop::lobby {

const std::string* HttpRequest::header(const std::string& lowerName) const {
    auto it = headers.find(lowerName);
    return it == headers.end() ? nullptr : &it->second;
}

namespace {

std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t')) --b;
    return s.substr(a, b - a);
}

} // namespace

ParseResult parseHttpRequest(const std::string& buffer) {
    ParseResult result;
    size_t headEnd = buffer.find("\r\n\r\n");
    if (headEnd == std::string::npos) {
        result.state = buffer.size() > kMaxHeaderBytes ? ParseState::TooLarge : ParseState::NeedMore;
        return result;
    }
    if (headEnd > kMaxHeaderBytes) {
        result.state = ParseState::TooLarge;
        return result;
    }
    HttpRequest req;
    size_t lineEnd = buffer.find("\r\n");
    std::string line = buffer.substr(0, lineEnd);
    size_t sp1 = line.find(' ');
    size_t sp2 = sp1 == std::string::npos ? std::string::npos : line.find(' ', sp1 + 1);
    if (sp1 == std::string::npos || sp2 == std::string::npos || sp1 == 0) {
        result.state = ParseState::Bad;
        return result;
    }
    req.method = line.substr(0, sp1);
    std::string target = line.substr(sp1 + 1, sp2 - sp1 - 1);
    std::string version = line.substr(sp2 + 1);
    if (version != "HTTP/1.1" && version != "HTTP/1.0") {
        result.state = ParseState::Bad;
        return result;
    }
    if (target.empty() || target[0] != '/') {
        result.state = ParseState::Bad;
        return result;
    }
    size_t q = target.find('?');
    req.path = target.substr(0, q);
    if (q != std::string::npos) req.query = target.substr(q + 1);

    size_t pos = lineEnd + 2;
    while (pos < headEnd) {
        size_t e = buffer.find("\r\n", pos);
        std::string h = buffer.substr(pos, e - pos);
        pos = e + 2;
        size_t colon = h.find(':');
        if (colon == std::string::npos || colon == 0) {
            result.state = ParseState::Bad;
            return result;
        }
        req.headers[lower(h.substr(0, colon))] = trim(h.substr(colon + 1));
    }
    if (req.header("transfer-encoding")) {
        result.state = ParseState::Bad;
        return result;
    }
    size_t length = 0;
    if (const std::string* cl = req.header("content-length")) {
        if (cl->empty() || cl->size() > 9) {
            result.state = ParseState::Bad;
            return result;
        }
        for (char c : *cl) {
            if (c < '0' || c > '9') {
                result.state = ParseState::Bad;
                return result;
            }
        }
        length = size_t(std::strtoull(cl->c_str(), nullptr, 10));
        if (length > kMaxBodyBytes) {
            result.state = ParseState::TooLarge;
            return result;
        }
    }
    size_t bodyStart = headEnd + 4;
    if (buffer.size() < bodyStart + length) {
        result.state = ParseState::NeedMore;
        return result;
    }
    req.body = buffer.substr(bodyStart, length);
    result.request = std::move(req);
    result.state = ParseState::Done;
    return result;
}

const char* reasonPhrase(int status) {
    switch (status) {
    case 200: return "OK";
    case 201: return "Created";
    case 204: return "No Content";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 409: return "Conflict";
    case 429: return "Too Many Requests";
    case 500: return "Internal Server Error";
    case 503: return "Service Unavailable";
    default: return "Status";
    }
}

std::string serializeHttpResponse(const HttpResponse& r) {
    std::string out = "HTTP/1.1 " + std::to_string(r.status) + " " + reasonPhrase(r.status) + "\r\n";
    if (r.status != 204) out += "Content-Type: application/json\r\n";
    out += "Content-Length: " + std::to_string(r.body.size()) + "\r\n";
    out += "Connection: close\r\n";
    for (const auto& h : r.headers) out += h.first + ": " + h.second + "\r\n";
    out += "\r\n";
    out += r.body;
    return out;
}

bool percentDecode(const std::string& in, std::string& out) {
    out.clear();
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] != '%') {
            out += in[i];
            continue;
        }
        if (i + 2 >= in.size()) return false;
        auto hex = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        int h = hex(in[i + 1]), l = hex(in[i + 2]);
        if (h < 0 || l < 0) return false;
        out += char(h * 16 + l);
        i += 2;
    }
    return true;
}

} // namespace sfcop::lobby
