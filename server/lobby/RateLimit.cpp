#include "RateLimit.h"

#include <algorithm>
#include <cmath>

namespace sfcop::lobby {

bool RateLimiter::allow(const std::string& source, int kind, int perMinute, uint64_t nowMs,
                        int* retryAfterSeconds) {
    lastSeen_[source] = nowMs;
    auto key = std::make_pair(source, kind);
    auto it = buckets_.find(key);
    if (it == buckets_.end()) {
        it = buckets_.emplace(key, Bucket{double(perMinute), nowMs}).first;
    }
    Bucket& b = it->second;
    double rate = double(perMinute) / 60000.0; // tokens per millisecond
    if (nowMs > b.updatedMs) {
        b.tokens = std::min(double(perMinute), b.tokens + double(nowMs - b.updatedMs) * rate);
        b.updatedMs = nowMs;
    }
    if (b.tokens >= 1.0) {
        b.tokens -= 1.0;
        return true;
    }
    if (retryAfterSeconds) {
        double ms = (1.0 - b.tokens) / rate;
        *retryAfterSeconds = std::max(1, int(std::ceil(ms / 1000.0)));
    }
    return false;
}

void RateLimiter::sweep(uint64_t nowMs) {
    for (auto it = lastSeen_.begin(); it != lastSeen_.end();) {
        if (nowMs >= it->second + 60000) {
            for (auto b = buckets_.begin(); b != buckets_.end();) {
                b = (b->first.first == it->first) ? buckets_.erase(b) : std::next(b);
            }
            it = lastSeen_.erase(it);
        } else {
            ++it;
        }
    }
}

} // namespace sfcop::lobby
