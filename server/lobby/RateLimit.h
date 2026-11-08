// Token buckets per (source, kind), for LOBBY_API.md "Rate limits". Time is
// passed in as milliseconds; the buckets of a source are dropped a minute after its last request.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <utility>

namespace sfcop::lobby {

class RateLimiter {
public:
    // Takes one token from the bucket (source, kind) that holds `perMinute` and refills evenly.
    // On refusal returns false and sets retryAfterSeconds (at least 1).
    bool allow(const std::string& source, int kind, int perMinute, uint64_t nowMs, int* retryAfterSeconds);

    // Forgets sources idle for 60 s or more.
    void sweep(uint64_t nowMs);

    size_t sourceCount() const { return lastSeen_.size(); }

private:
    struct Bucket {
        double tokens = 0;
        uint64_t updatedMs = 0;
    };
    std::map<std::pair<std::string, int>, Bucket> buckets_;
    std::map<std::string, uint64_t> lastSeen_;
};

} // namespace sfcop::lobby
