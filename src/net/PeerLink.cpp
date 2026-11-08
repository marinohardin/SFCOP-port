#include "net/PeerLink.h"

#include <algorithm>

namespace sfcop::net {
namespace {

using platform::Error;

constexpr std::uint32_t kAckDelayMs = 50;
constexpr std::uint32_t kMinRetransmitMs = 200;
constexpr std::uint32_t kMaxRetransmitMs = 2000;
constexpr std::uint32_t kDeadAfterMs = 10000;
constexpr std::uint32_t kKeepaliveMs = 1000;
constexpr std::uint32_t kReassemblyGiveUpMs = 5000;
constexpr std::size_t kMaxPartials = 16;
constexpr std::size_t kMaxPartialBytes = 256 * 1024;
constexpr std::size_t kMaxUnacked = 256;
constexpr std::uint32_t kMaxReliableAhead = 256;
// Reliable messages held out of order, in bytes: past it one is dropped unacknowledged, and the
// sender's retransmission brings it again once the gap before it has closed.
constexpr std::size_t kMaxHeldBytes = 256 * 1024;
constexpr std::size_t kMaxQueuedMessages = 1024;

// RFC 1982: a is later than b.
bool later(std::uint32_t a, std::uint32_t b) {
    std::uint32_t d = a - b;
    return d >= 1 && d <= 0x7FFFFFFFu;
}

// True once `now` has reached `due`, on a clock that wraps.
bool reached(std::uint32_t now, std::uint32_t due) {
    return static_cast<std::int32_t>(now - due) >= 0;
}

} // namespace

bool PeerLink::Window::Fresh(std::uint32_t sequence) const {
    if (!any_ || later(sequence, highest_)) {
        return true;
    }
    std::uint32_t back = highest_ - sequence;
    return back < seen_.size() && !seen_[back];
}

void PeerLink::Window::Mark(std::uint32_t sequence) {
    if (!any_) {
        any_ = true;
        highest_ = sequence;
        seen_.reset();
        seen_[0] = true;
        return;
    }
    if (later(sequence, highest_)) {
        std::uint32_t ahead = sequence - highest_;
        if (ahead >= seen_.size()) {
            seen_.reset();
        } else {
            seen_ <<= ahead;
        }
        seen_[0] = true;
        highest_ = sequence;
        return;
    }
    std::uint32_t back = highest_ - sequence;
    if (back < seen_.size()) {
        seen_[back] = true;
    }
}

PeerLink::PeerLink(std::uint64_t sessionId, std::uint32_t self, std::uint32_t peer,
                   std::uint32_t nowMs)
    : session_(sessionId), self_(self), peer_(peer), lastSentMs_(nowMs), lastReceivedMs_(nowMs) {}

EnvelopeHeader PeerLink::header(EnvelopeKind kind, bool reliable, std::uint32_t sequence) const {
    EnvelopeHeader out;
    out.kind = kind;
    out.flags = reliable ? kEnvelopeReliable : 0;
    out.sessionId = session_;
    out.source = self_;
    out.destination = peer_;
    out.sequence = sequence;
    return out;
}

void PeerLink::emit(Route route, const Envelope &envelope, std::uint32_t nowMs,
                    std::vector<OutgoingEnvelope> &out) {
    out.push_back(OutgoingEnvelope{route, encodeEnvelope(envelope)});
    lastSentMs_ = nowMs;
}

std::uint32_t PeerLink::retransmitTimeout() const {
    return std::min(kMaxRetransmitMs, std::max(kMinRetransmitMs, 2 * srttMs_));
}

platform::Status PeerLink::Send(const std::uint8_t *data, std::size_t bytes, bool reliable,
                                std::uint32_t nowMs, std::vector<OutgoingEnvelope> &out) {
    if (bytes == 0 || bytes > kMaxLinkMessage) {
        return Error::kInvalidArgument;
    }
    if (closing_ || peerClosed_ || Dead(nowMs)) {
        return Error::kConnectionReset;
    }
    if (reliable && unacked_.size() >= kMaxUnacked) {
        return Error::kWouldBlock;
    }
    const std::size_t count = (bytes + kMaxFragmentPayload - 1) / kMaxFragmentPayload;
    const std::uint32_t sequence = reliable ? nextReliable_++ : nextUnreliable_++;
    Unacked pending;
    for (std::size_t index = 0; index < count; ++index) {
        Envelope envelope;
        envelope.header = header(EnvelopeKind::kData, reliable, sequence);
        envelope.header.fragmentIndex = static_cast<std::uint16_t>(index);
        envelope.header.fragmentCount = static_cast<std::uint16_t>(count);
        std::size_t offset = index * kMaxFragmentPayload;
        std::size_t length = std::min(kMaxFragmentPayload, bytes - offset);
        envelope.payload.assign(data + offset, data + offset + length);
        emit(Route::kChosen, envelope, nowMs, out);
        if (reliable) {
            pending.envelopes.push_back(out.back().bytes);
        }
    }
    if (reliable) {
        pending.firstSentMs = nowMs;
        pending.lastSentMs = nowMs;
        pending.timeoutMs = retransmitTimeout();
        unacked_.emplace(sequence, std::move(pending));
    }
    return {};
}

void PeerLink::Close(std::uint32_t reason, std::uint32_t nowMs,
                     std::vector<OutgoingEnvelope> &out) {
    if (closing_) {
        return;
    }
    closing_ = true;
    Envelope envelope;
    const std::uint32_t sequence = nextReliable_++;
    envelope.header = header(EnvelopeKind::kClose, true, sequence);
    envelope.reason = reason;
    emit(Route::kChosen, envelope, nowMs, out);
    Unacked pending;
    pending.envelopes.push_back(out.back().bytes);
    pending.firstSentMs = nowMs;
    pending.lastSentMs = nowMs;
    pending.timeoutMs = retransmitTimeout();
    unacked_.emplace(sequence, std::move(pending));
}

std::uint32_t PeerLink::Ping(Route route, std::uint32_t nowMs, std::vector<OutgoingEnvelope> &out) {
    Envelope envelope;
    envelope.header = header(EnvelopeKind::kPing, false, nextUnreliable_++);
    envelope.nonce = nextNonce_++;
    envelope.sentMs = nowMs;
    emit(route, envelope, nowMs, out);
    return envelope.nonce;
}

void PeerLink::sendAck(std::uint32_t nowMs, std::vector<OutgoingEnvelope> &out) {
    Envelope envelope;
    envelope.header = header(EnvelopeKind::kAck, false, nextUnreliable_++);
    envelope.cumulative = nextExpected_ - 1;
    for (std::uint32_t bit = 0; bit < 32; ++bit) {
        if (held_.count(envelope.cumulative + 1 + bit) != 0) {
            envelope.mask |= 1u << bit;
        }
    }
    emit(Route::kChosen, envelope, nowMs, out);
    ackDue_ = false;
}

void PeerLink::takeAck(const Envelope &envelope) {
    for (auto it = unacked_.begin(); it != unacked_.end();) {
        std::uint32_t sequence = it->first;
        std::uint32_t past = sequence - envelope.cumulative - 1;
        bool acked = !later(sequence, envelope.cumulative) ||
                     (past < 32 && (envelope.mask & (1u << past)) != 0);
        it = acked ? unacked_.erase(it) : std::next(it);
    }
}

void PeerLink::accept(EnvelopeKind kind, std::vector<std::uint8_t> payload) {
    if (kind == EnvelopeKind::kClose) {
        peerClosed_ = true;
        return;
    }
    if (messages_.size() < kMaxQueuedMessages) {
        messages_.push_back(std::move(payload));
    }
}

bool PeerLink::reassemble(const Envelope &envelope, bool reliable, std::uint32_t nowMs,
                          std::vector<std::uint8_t> &whole) {
    const EnvelopeHeader &h = envelope.header;
    const std::uint64_t key = (static_cast<std::uint64_t>(h.sequence) << 1) | (reliable ? 1 : 0);
    auto found = partials_.find(key);
    if (found == partials_.end()) {
        if (partials_.size() >= kMaxPartials ||
            partialBytes_ + envelope.payload.size() > kMaxPartialBytes) {
            return false;
        }
        Partial partial;
        partial.count = h.fragmentCount;
        partial.firstMs = nowMs;
        partial.fragments.resize(h.fragmentCount);
        found = partials_.emplace(key, std::move(partial)).first;
    }
    Partial &partial = found->second;
    if (partial.count != h.fragmentCount || !partial.fragments[h.fragmentIndex].empty() ||
        partialBytes_ + envelope.payload.size() > kMaxPartialBytes) {
        return false;
    }
    partial.fragments[h.fragmentIndex] = envelope.payload;
    partial.bytes += envelope.payload.size();
    partialBytes_ += envelope.payload.size();
    if (++partial.received < partial.count) {
        return false;
    }
    whole.clear();
    whole.reserve(partial.bytes);
    for (const std::vector<std::uint8_t> &fragment : partial.fragments) {
        whole.insert(whole.end(), fragment.begin(), fragment.end());
    }
    partialBytes_ -= partial.bytes;
    partials_.erase(found);
    return true;
}

void PeerLink::deliver(EnvelopeKind kind, bool reliable, std::uint32_t sequence,
                       std::vector<std::uint8_t> payload, std::uint32_t nowMs,
                       std::vector<OutgoingEnvelope> &out) {
    if (!reliable) {
        window_.Mark(sequence);
        accept(kind, std::move(payload));
        return;
    }
    if (sequence != nextExpected_) {
        if (heldBytes_ + payload.size() > kMaxHeldBytes) {
            return;
        }
        heldBytes_ += payload.size();
        held_[sequence] = Held{kind, std::move(payload)};
        sendAck(nowMs, out); // out of order: acknowledge at once
        return;
    }
    accept(kind, std::move(payload));
    ++nextExpected_;
    for (auto next = held_.find(nextExpected_); next != held_.end();
         next = held_.find(nextExpected_)) {
        heldBytes_ -= next->second.payload.size();
        accept(next->second.kind, std::move(next->second.payload));
        held_.erase(next);
        ++nextExpected_;
    }
    if (!held_.empty()) {
        sendAck(nowMs, out);
    } else if (!ackDue_) {
        ackDue_ = true;
        ackDueMs_ = nowMs + kAckDelayMs;
    }
}

bool PeerLink::Receive(const Envelope &envelope, Route arrivedOn, std::uint32_t nowMs,
                       std::vector<OutgoingEnvelope> &out) {
    const EnvelopeHeader &h = envelope.header;
    if (h.sessionId != session_ || h.destination != self_ || h.source != peer_) {
        return false;
    }
    const bool reliable = (h.flags & kEnvelopeReliable) != 0 &&
                          (h.kind == EnvelopeKind::kData || h.kind == EnvelopeKind::kClose);
    if (reliable) {
        if (!later(h.sequence, nextExpected_ - 1) || held_.count(h.sequence) != 0) {
            lastReceivedMs_ = nowMs;
            sendAck(nowMs, out); // a duplicate: its ack was lost
            return false;
        }
        if (h.sequence - nextExpected_ >= kMaxReliableAhead) {
            return false;
        }
    } else if (!window_.Fresh(h.sequence)) {
        return false;
    }
    lastReceivedMs_ = nowMs;
    switch (h.kind) {
    case EnvelopeKind::kAck:
        window_.Mark(h.sequence);
        takeAck(envelope);
        return true;
    case EnvelopeKind::kPing: {
        window_.Mark(h.sequence);
        Envelope pong;
        pong.header = header(EnvelopeKind::kPong, false, nextUnreliable_++);
        pong.nonce = envelope.nonce;
        pong.sentMs = envelope.sentMs;
        emit(arrivedOn == Route::kRelay ? Route::kRelay : Route::kDirect, pong, nowMs, out);
        return true;
    }
    case EnvelopeKind::kPong: {
        window_.Mark(h.sequence);
        (arrivedOn == Route::kRelay ? pongRelay_ : pongDirect_) = true;
        std::uint32_t sample = nowMs - envelope.sentMs;
        if (sample < 60000) {
            srttMs_ =
                srttMs_ == 0 ? std::max<std::uint32_t>(sample, 1) : (7 * srttMs_ + sample) / 8;
        }
        return true;
    }
    case EnvelopeKind::kData:
    case EnvelopeKind::kClose:
        break;
    }
    if (h.fragmentCount > 1) {
        std::vector<std::uint8_t> whole;
        if (reassemble(envelope, reliable, nowMs, whole)) {
            deliver(h.kind, reliable, h.sequence, std::move(whole), nowMs, out);
        }
        return true;
    }
    deliver(h.kind, reliable, h.sequence, envelope.payload, nowMs, out);
    return true;
}

void PeerLink::Tick(std::uint32_t nowMs, std::vector<OutgoingEnvelope> &out) {
    for (auto it = partials_.begin(); it != partials_.end();) {
        if (nowMs - it->second.firstMs >= kReassemblyGiveUpMs) {
            partialBytes_ -= it->second.bytes;
            it = partials_.erase(it);
        } else {
            ++it;
        }
    }
    if (ackDue_ && reached(nowMs, ackDueMs_)) {
        sendAck(nowMs, out);
    }
    for (auto &[sequence, pending] : unacked_) {
        (void)sequence;
        if (nowMs - pending.firstSentMs >= kDeadAfterMs) {
            reliableDead_ = true;
            continue;
        }
        if (nowMs - pending.lastSentMs >= pending.timeoutMs) {
            for (const std::vector<std::uint8_t> &bytes : pending.envelopes) {
                out.push_back(OutgoingEnvelope{Route::kChosen, bytes});
            }
            lastSentMs_ = nowMs;
            pending.lastSentMs = nowMs;
            pending.timeoutMs = std::min(kMaxRetransmitMs, pending.timeoutMs * 2);
        }
    }
    if (!closing_ && !peerClosed_ && nowMs - lastSentMs_ >= kKeepaliveMs) {
        Ping(Route::kChosen, nowMs, out);
    }
}

std::vector<std::uint8_t> PeerLink::TakeMessage() {
    std::vector<std::uint8_t> message = std::move(messages_.front());
    messages_.pop_front();
    return message;
}

bool PeerLink::Dead(std::uint32_t nowMs) const {
    return reliableDead_ || nowMs - lastReceivedMs_ >= kDeadAfterMs;
}

bool PeerLink::CloseFinished(std::uint32_t nowMs) const {
    return closing_ && (unacked_.empty() || Dead(nowMs));
}

} // namespace sfcop::net
