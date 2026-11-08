// Just enough HTTP/1.1 for the lobby's API: one request per connection, a request line, headers
// and a Content-Length body. TLS is not here: the host's reverse proxy ends it (server/README.md).
#pragma once

#include <map>
#include <string>

namespace sfcop::lobby {

constexpr size_t kMaxBodyBytes = 4096;   // LOBBY_API.md, "Bodies"
constexpr size_t kMaxHeaderBytes = 8192;

struct HttpRequest {
    std::string method;
    std::string path;                           // without the query
    std::string query;                          // after '?', not decoded
    std::map<std::string, std::string> headers; // names lower-cased
    std::string body;
    std::string source;                         // the client's address, as text (not from the wire text)

    const std::string* header(const std::string& lowerName) const;
};

struct HttpResponse {
    int status = 200;
    std::string body;
    std::map<std::string, std::string> headers;
    std::string pattern; // the route as logged, e.g. "/v1/sessions/<session>"; not sent
};

enum class ParseState { NeedMore, Done, Bad, TooLarge };

struct ParseResult {
    ParseState state = ParseState::NeedMore;
    HttpRequest request; // valid when Done
};

// Parses what has arrived so far. Done when the whole request, body included, is in `buffer`.
ParseResult parseHttpRequest(const std::string& buffer);

std::string serializeHttpResponse(const HttpResponse& response);
const char* reasonPhrase(int status);

// %XX decoding of a query value ('+' stays a plus); false on a bad escape.
bool percentDecode(const std::string& in, std::string& out);

} // namespace sfcop::lobby
