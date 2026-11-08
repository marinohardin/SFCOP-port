#include "RelayService.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "Sha256.h"

namespace sfcop::relay {
namespace {

std::string hex(std::uint64_t v, int digits) {
    char buf[20];
    std::snprintf(buf, sizeof buf, "%0*llx", digits, static_cast<unsigned long long>(v));
    return buf;
}

// Debug lines only: addresses are identifying.
std::string describe(const Endpoint& e) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%u.%u.%u.%u:%u", (e.address >> 24) & 255, (e.address >> 16) & 255,
                  (e.address >> 8) & 255, e.address & 255, e.port);
    return buf;
}

constexpr std::uint64_t kPendingDelayMs = 100;
constexpr std::uint64_t kSourceIdleMs = 120000;
constexpr std::uint64_t kIntroductionKeepMs = 10000;
constexpr std::uint64_t kMinuteMs = 60000;

}  // namespace

RelayService::RelayService(RelayConfig config, Sender& sender, const TimeSource& time,
                           const Logger& log, Random random)
    : config_(std::move(config)), sender_(sender), time_(time), log_(log), random_(std::move(random)) {}

void RelayService::refill(Bucket& b, std::uint64_t now, double ratePerSec, double capacity) {
    if (!b.primed) {
        b.primed = true;
        b.tokens = capacity;
        b.lastMs = now;
        return;
    }
    if (now > b.lastMs) {
        b.tokens = std::min(capacity, b.tokens + double(now - b.lastMs) * ratePerSec / 1000.0);
        b.lastMs = now;
    }
}

bool RelayService::expired(const Binding& b) const {
    return time_.nowMs() - b.lastSeenMs >= config_.bindingTimeoutMs;
}

RelayService::Binding* RelayService::find(std::uint64_t session, std::uint32_t peer) {
    auto s = sessions_.find(session);
    if (s == sessions_.end()) return nullptr;
    auto p = s->second.find(peer);
    if (p == s->second.end() || expired(p->second)) return nullptr;
    return &p->second;
}

void RelayService::setBlocklist(std::unordered_set<std::uint32_t> addresses) {
    blocklist_ = std::move(addresses);
}

void RelayService::onDatagram(const Endpoint& from, const std::uint8_t* data, std::size_t size) {
    if (blocklist_.count(from.address)) {
        ++stats_.refusedBlocked;
        logDebug("block: dropped a datagram from " + describe(from));
        return;
    }
    switch (peekMagic(data, size)) {
        case kRvMagic: handleRendezvous(from, data, size); break;
        case kEnvelopeMagic: handleEnvelope(from, data, size); break;
        default: ++stats_.refusedUnknownKind; break;
    }
}

bool RelayService::validTicketMac(const Ticket& t) const {
    std::uint8_t body[20];
    encodeTicketBody(t, body);
    Digest mac = hmacSha256(config_.ticketKey.data(), config_.ticketKey.size(), body, sizeof body);
    return constantTimeEqual(mac.data(), t.mac, sizeof t.mac);
}

RelayService::Allow RelayService::takeSourceToken(const Endpoint& from, std::uint8_t type) {
    std::uint64_t now = time_.nowMs();
    auto it = sources_.find(from.address);
    if (it == sources_.end()) {
        if (sources_.size() >= config_.maxTrackedSources && !lru_.empty()) {
            sources_.erase(lru_.back());  // the address idle the longest makes room
            lru_.pop_back();
        }
        lru_.push_front(from.address);
        it = sources_.emplace(from.address, SourceLimits{}).first;
        it->second.lruPosition = lru_.begin();
    } else {
        lru_.splice(lru_.begin(), lru_, it->second.lruPosition);
    }
    SourceLimits& s = it->second;
    s.lastSeenMs = now;
    double allCap = config_.rendezvousPerMinute;
    refill(s.all, now, allCap / 60.0, allCap);
    if (s.all.tokens < 1) {
        ++stats_.rateLimitedRendezvous;
        logDebug("rate limit: rendezvous messages from " + describe(from));
        return Allow::kDropSilently;
    }
    s.all.tokens -= 1;
    Bucket* typed = nullptr;
    double cap = 0;
    if (type == kBind) {
        typed = &s.binds;
        cap = config_.bindsPerMinute;
    } else if (type == kConnect) {
        typed = &s.connects;
        cap = config_.connectsPerMinute;
    }
    if (typed) {
        refill(*typed, now, cap / 60.0, cap);
        if (typed->tokens < 1) {
            ++stats_.rateLimitedRendezvous;
            logDebug("rate limit: a message type from " + describe(from));
            return Allow::kReplyRateLimited;
        }
        typed->tokens -= 1;
    }
    return Allow::kYes;
}

void RelayService::sendError(const Endpoint& to, std::uint32_t transaction, std::uint32_t result) {
    Bytes b = encodeRvError(transaction, result);
    ++stats_.rendezvousErrors;
    sender_.send(to, b.data(), b.size());
}

void RelayService::handleRendezvous(const Endpoint& from, const std::uint8_t* data,
                                    std::size_t size) {
    RvHeader h;
    if (!decodeRvHeader(data, size, h)) {
        ++stats_.refusedMalformed;
        return;
    }
    // Past the total limit the request is dropped unanswered, so a flood cannot be reflected.
    // Past a type's limit the answer is a 16-byte RvError to a 12-byte-or-longer request.
    Allow allow = takeSourceToken(from, h.type);
    if (allow == Allow::kDropSilently) return;
    if (allow == Allow::kReplyRateLimited) {
        sendError(from, h.transaction, kRateLimited);
        return;
    }
    if (h.version != kRvVersion) {
        // Only Bind and Connect get an answer in any version; Unbind has no reply by the
        // document and the rest are not requests, so those are dropped and nothing is reflected
        // to a spoofed source.
        if (h.type == kBind || h.type == kConnect) sendError(from, h.transaction, kUnsupportedVersion);
        return;
    }
    switch (h.type) {
        case kBind: {
            BindMessage m;
            if (decodeBind(data, size, m)) handleBind(from, m); else ++stats_.refusedMalformed;
            break;
        }
        case kConnect: {
            ConnectMessage m;
            if (decodeConnect(data, size, m)) handleConnect(from, m); else ++stats_.refusedMalformed;
            break;
        }
        case kUnbind: {
            UnbindMessage m;
            if (decodeUnbind(data, size, m)) handleUnbind(from, m); else ++stats_.refusedMalformed;
            break;
        }
        default:
            // Service-to-client and peer-to-peer types are not accepted here.
            ++stats_.refusedMalformed;
            break;
    }
}

void RelayService::handleBind(const Endpoint& from, const BindMessage& m) {
    const Ticket& t = m.ticket;
    bool roleOk = (t.role == 1 && t.peerId == 1) || (t.role == 2 && t.peerId >= 2);
    if (!validTicketMac(t) || !roleOk) {
        sendError(from, m.header.transaction, kBadTicket);
        logDebug("bind refused: bad ticket from " + describe(from));
        return;
    }
    for (const Candidate& c : m.candidates) {
        // Not an address a peer could be reached at. Loopback stays allowed: two clients on one
        // machine, and the container test, use it.
        bool multicast = (c.address >> 28) == 0xE;
        if (c.address == 0 || c.address == 0xFFFFFFFFu || multicast || c.port == 0) {
            ++stats_.refusedMalformed;
            return;
        }
    }
    std::uint64_t now = time_.nowMs();
    bool ticketExpired = time_.unixSeconds() > t.expiresAt;

    PeerMap& peers = sessions_[t.sessionId];
    auto it = peers.find(t.peerId);
    bool live = it != peers.end() && !expired(it->second);
    bool sameAddress = live && it->second.address == from;

    if (!sameAddress && ticketExpired) {
        if (peers.empty()) sessions_.erase(t.sessionId);
        sendError(from, m.header.transaction, kTicketExpired);
        return;
    }
    if (!live && it != peers.end()) {  // a stale entry of this peer: forget it before counting
        dropBinding(peers, it);
        it = peers.end();
    }
    if (it == peers.end()) {
        if (total_ >= config_.maxBindings) {
            if (peers.empty()) sessions_.erase(t.sessionId);
            sendError(from, m.header.transaction, kFull);
            return;
        }
        it = peers.emplace(t.peerId, Binding{}).first;
        ++total_;
    }
    Binding& b = it->second;
    if (!sameAddress) {
        b.address = from;
        b.datagrams = Bucket{};
        b.bytes = Bucket{};
    }
    b.lastSeenMs = now;
    b.locals.clear();
    for (const Candidate& c : m.candidates) {
        if (c.kind == kLocal) b.locals.push_back(c);
    }
    bindingCount_.store(total_);
    ++stats_.bindsAccepted;

    Bytes reply = encodeBindReply(m.header.transaction, kOk,
                                  Candidate{from.address, from.port, kReflexive},
                                  config_.refreshSeconds);
    sender_.send(from, reply.data(), reply.size());
}

void RelayService::handleConnect(const Endpoint& from, const ConnectMessage& m) {
    Binding* a = find(m.sessionId, m.source);
    if (!a || !(a->address == from)) {
        // A session id is a 64-bit random value, so saying that none is bound reveals nothing.
        sendError(from, m.header.transaction,
                  sessions_.count(m.sessionId) ? kUnknownBinding : kUnknownSession);
        return;
    }
    Binding* b = (m.target == m.source) ? nullptr : find(m.sessionId, m.target);
    if (!b) {
        sendError(from, m.header.transaction, kUnknownPeer);
        return;
    }
    std::uint64_t now = time_.nowMs();
    auto key = std::make_tuple(m.sessionId, std::min(m.source, m.target), std::max(m.source, m.target));
    Introduction& intro = introductions_[key];
    if (intro.punchId == 0 || now - intro.atMs >= config_.introductionReuseMs) {
        std::uint32_t id = 0;
        while (id == 0) id = random_();
        intro.punchId = id;
        intro.atMs = now;
    }
    auto candidatesOf = [](const Binding& x) {
        std::vector<Candidate> c;
        c.push_back({x.address.address, x.address.port, kReflexive});
        c.insert(c.end(), x.locals.begin(), x.locals.end());
        return c;
    };
    Bytes toA = encodePeerCandidates(m.header.transaction, m.sessionId, m.target, intro.punchId,
                                     candidatesOf(*b));
    Bytes toB = encodePeerCandidates(0, m.sessionId, m.source, intro.punchId, candidatesOf(*a));
    Endpoint addrA = a->address, addrB = b->address;
    sender_.send(addrA, toA.data(), toA.size());
    sender_.send(addrB, toB.data(), toB.size());
    pending_.push_back({now + kPendingDelayMs, addrA, std::move(toA)});
    pending_.push_back({now + kPendingDelayMs, addrB, std::move(toB)});
    ++stats_.introductions;
}

void RelayService::handleUnbind(const Endpoint& from, const UnbindMessage& m) {
    auto s = sessions_.find(m.sessionId);
    if (s == sessions_.end()) return;
    auto p = s->second.find(m.peer);
    if (p == s->second.end() || !(p->second.address == from)) return;
    dropBinding(s->second, p);
    if (s->second.empty()) sessions_.erase(s);
}

void RelayService::handleEnvelope(const Endpoint& from, const std::uint8_t* data,
                                  std::size_t size) {
    EnvelopeHeader h;
    if (size > kMaxEnvelopeSize || !decodeEnvelopeHeader(data, size, h) ||
        h.version != kEnvelopeVersion) {
        ++stats_.refusedBadEnvelope;
        return;
    }
    Binding* src = find(h.sessionId, h.source);
    Binding* dst = find(h.sessionId, h.destination);
    if (!src || !dst || !(src->address == from) || h.source == h.destination) {
        // No error goes back: a sender must not learn which sessions exist.
        ++stats_.refusedNotBound;
        return;
    }
    std::uint64_t now = time_.nowMs();
    double dgramCap = config_.relayDatagramsPerSecond;
    double byteCap = config_.relayBytesPerSecond;
    refill(src->datagrams, now, dgramCap, dgramCap);
    refill(src->bytes, now, byteCap, byteCap);
    if (src->datagrams.tokens < 1 || src->bytes.tokens < double(size)) {
        ++stats_.rateLimitedRelay;
        return;
    }
    src->datagrams.tokens -= 1;
    src->bytes.tokens -= double(size);
    sender_.send(dst->address, data, size);
    ++stats_.forwardedDatagrams;
    stats_.forwardedBytes += size;
}

void RelayService::dropBinding(PeerMap& peers, PeerMap::iterator it) {
    peers.erase(it);
    --total_;
    bindingCount_.store(total_);
}

std::size_t RelayService::removeSession(std::uint64_t sessionId) {
    auto s = sessions_.find(sessionId);
    if (s == sessions_.end()) return 0;
    std::size_t n = s->second.size();
    total_ -= n;
    sessions_.erase(s);
    bindingCount_.store(total_);
    logDebug("session " + hex(sessionId, 16) + " removed by the lobby, " + std::to_string(n) +
             " bindings");
    return n;
}

void RelayService::tick() {
    std::uint64_t now = time_.nowMs();

    for (auto s = sessions_.begin(); s != sessions_.end();) {
        for (auto p = s->second.begin(); p != s->second.end();) {
            if (expired(p->second)) {
                p = s->second.erase(p);
                --total_;
            } else {
                ++p;
            }
        }
        s = s->second.empty() ? sessions_.erase(s) : std::next(s);
    }
    bindingCount_.store(total_);

    std::vector<Pending> keep;
    for (Pending& p : pending_) {
        if (p.dueMs <= now) {
            sender_.send(p.to, p.datagram.data(), p.datagram.size());
        } else {
            keep.push_back(std::move(p));
        }
    }
    pending_ = std::move(keep);

    for (auto i = introductions_.begin(); i != introductions_.end();) {
        i = (now - i->second.atMs >= kIntroductionKeepMs) ? introductions_.erase(i) : std::next(i);
    }
    for (auto i = sources_.begin(); i != sources_.end();) {
        if (now - i->second.lastSeenMs >= kSourceIdleMs) {
            lru_.erase(i->second.lruPosition);
            i = sources_.erase(i);
        } else {
            ++i;
        }
    }

    if (!minuteStarted_) {
        minuteStarted_ = true;
        lastMinuteMs_ = now;
        return;
    }
    if (now - lastMinuteMs_ >= kMinuteMs) {
        const RelayStats& a = stats_;
        const RelayStats& r = lastReported_;
        log_.info("minute: bindings " + std::to_string(total_) + ", forwarded " +
                  std::to_string(a.forwardedDatagrams - r.forwardedDatagrams) + " datagrams and " +
                  std::to_string(a.forwardedBytes - r.forwardedBytes) + " bytes, refused " +
                  std::to_string((a.refusedUnknownKind - r.refusedUnknownKind) +
                                 (a.refusedMalformed - r.refusedMalformed)) +
                  " as malformed, " + std::to_string(a.refusedBadEnvelope - r.refusedBadEnvelope) +
                  " as bad envelopes, " + std::to_string(a.refusedNotBound - r.refusedNotBound) +
                  " as not bound, " + std::to_string(a.refusedBlocked - r.refusedBlocked) +
                  " as blocked, rate limited " +
                  std::to_string(a.rateLimitedRelay - r.rateLimitedRelay) + " relayed and " +
                  std::to_string(a.rateLimitedRendezvous - r.rateLimitedRendezvous) +
                  " rendezvous");
        lastReported_ = stats_;
        lastMinuteMs_ = now;
    }
}

}  // namespace sfcop::relay
