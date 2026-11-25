#include "xlink.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <ctime>

namespace {
constexpr uint32_t maxPayload = 64u << 20;

void put32(uint8_t *out, uint32_t v) { for (int i = 0; i < 4; i++) out[i] = uint8_t(v >> (8 * i)); }
uint32_t get32(const uint8_t *in) { return uint32_t(in[0]) | uint32_t(in[1]) << 8 | uint32_t(in[2]) << 16 | uint32_t(in[3]) << 24; }

std::chrono::steady_clock::time_point after(double seconds) {
    return std::chrono::steady_clock::now() + std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(seconds));
}
}

void XLinkConnection::Header::encode(uint8_t *out) const {
    memset(out, 0, length);
    put32(out, uint32_t(id));
    put32(out + 4, uint32_t(type));
    memcpy(out + 8, name.data(), std::min<size_t>(name.size(), 63));
    put32(out + 72, streamId);
    put32(out + 76, size);
    put32(out + 80, flags);
}

XLinkConnection::Header XLinkConnection::Header::decode(const uint8_t *in) {
    Header h;
    h.id = int32_t(get32(in));
    h.type = int32_t(get32(in + 4));
    h.name = std::string(reinterpret_cast<const char *>(in + 8), strnlen(reinterpret_cast<const char *>(in + 8), 64));
    h.streamId = get32(in + 72);
    h.size = get32(in + 76);
    h.flags = get32(in + 80);
    return h;
}

std::unique_ptr<XLinkConnection> XLinkConnection::connect(OpalPID pid, ConnectError &error, double pingTimeout,
                                                          Log onLog, std::function<void()> onDeath) {
    USBPipe::OpenError openError;
    auto pipe = USBPipe::open(pid, openError);
    if (!pipe) {
        error = openError == USBPipe::OpenError::notFound ? ConnectError::notFound
              : openError == USBPipe::OpenError::busy ? ConnectError::busy
              : openError == USBPipe::OpenError::permission ? ConnectError::permission : ConnectError::notListening;
        return nullptr;
    }
    std::unique_ptr<XLinkConnection> link(new XLinkConnection(std::move(pipe), std::move(onLog)));
    Header ping;
    ping.type = pingReq;
    switch (link->exchange(ping, nullptr, pingTimeout, nullptr)) {
    case Outcome::response:
        error = ConnectError::none;
        { std::lock_guard guard(link->state_); link->onDeath_ = std::move(onDeath); }
        return link;
    case Outcome::sendFailed: link->close(); error = ConnectError::notListening; return nullptr;
    case Outcome::timedOut: link->close(); error = ConnectError::noAnswer; return nullptr;
    }
    return nullptr;
}

XLinkConnection::XLinkConnection(std::unique_ptr<USBPipe> pipe, Log onLog) : pipe_(std::move(pipe)), onLog_(std::move(onLog)) {
    reader_ = std::thread([this] { readLoop(); });
}

XLinkConnection::~XLinkConnection() { close(); }

bool XLinkConnection::isAlive() {
    std::lock_guard guard(state_);
    return alive_ && running_;
}

void XLinkConnection::close() {
    {
        std::lock_guard guard(state_);
        if (closed_) return;
        closed_ = true;
        running_ = false;
        onDeath_ = nullptr;
    }
    changed_.notify_all();
    pipe_->abortRead();   // stays signalled, so the reader can't miss it
    if (reader_.joinable()) reader_.join();
    if (watchdog_.joinable()) watchdog_.join();
    // Under the send lock, so nothing can be mid-write on a released interface.
    std::lock_guard guard(sendLock_);
    pipe_->close();
}

void XLinkConnection::feedWatchdog(uint32_t stream) {
    watchdog_ = std::thread([this, stream] {
        const Bytes keepalive = {0, 0, 0, 0};
        while (isAlive()) {
            write(stream, keepalive, 1);
            std::unique_lock guard(state_);
            changed_.wait_for(guard, std::chrono::milliseconds(500), [this] { return !running_ || !alive_; });
        }
    });
}

std::optional<uint32_t> XLinkConnection::open(const std::string &name, uint32_t size) {
    uint32_t local;
    {
        std::lock_guard guard(state_);
        auto it = streams_.find(name);
        local = it != streams_.end() ? it->second : freeId(streams_);
        streams_[name] = local;
    }
    Header h;
    h.type = createStreamReq;
    h.name = name;
    h.streamId = local;
    h.size = (size + 63) & ~63u;
    Header r;
    if (exchange(h, nullptr, 3, &r) != Outcome::response || (r.flags & 1) != 1) return {};
    if (onLog_) onLog_("xlink: opened " + name + " as stream " + std::to_string(r.streamId) + " (asked for " + std::to_string(local) + ")");
    std::lock_guard guard(state_);
    streams_[name] = r.streamId;
    hostStreams_.insert(r.streamId);
    return r.streamId;
}

bool XLinkConnection::write(uint32_t stream, const Bytes &data, double timeout) {
    Header h;
    h.type = writeReq;
    h.streamId = stream;
    h.size = uint32_t(data.size());
    Header r;
    if (exchange(h, &data, timeout, &r) != Outcome::response) return false;
    return (r.flags & 1) == 1;
}

std::optional<Bytes> XLinkConnection::read(uint32_t stream, double timeout) {
    auto deadline = after(timeout);
    std::unique_lock guard(state_);
    while (packets_[stream].empty()) {
        if (!alive_ || !running_) return {};
        if (changed_.wait_until(guard, deadline) == std::cv_status::timeout && packets_[stream].empty()) return {};
    }
    Bytes packet = std::move(packets_[stream].front());
    packets_[stream].pop_front();
    return packet;
}

// MARK: - Requests

XLinkConnection::Outcome XLinkConnection::exchange(Header h, const Bytes *payload, double timeout, Header *response) {
    h.id = nextRequestId();
    { std::lock_guard guard(state_); outstanding_.insert(h.id); }
    if (!send(h, payload, timeout)) {
        { std::lock_guard guard(state_); outstanding_.erase(h.id); }
        markDead();
        return Outcome::sendFailed;
    }
    auto deadline = after(timeout);
    std::unique_lock guard(state_);
    while (!responses_.count(h.id)) {
        if (!alive_ || !running_ || (changed_.wait_until(guard, deadline) == std::cv_status::timeout && !responses_.count(h.id))) {
            outstanding_.erase(h.id);
            return Outcome::timedOut;
        }
    }
    outstanding_.erase(h.id);
    if (response) *response = responses_[h.id];
    responses_.erase(h.id);
    return Outcome::response;
}

bool XLinkConnection::send(const Header &h, const Bytes *payload, double timeout) {
    std::lock_guard guard(sendLock_);
    uint8_t encoded[Header::length];
    h.encode(encoded);
    if (!pipe_->write(encoded, Header::length, 2000)) return false;
    if (payload && !payload->empty()) return pipe_->write(payload->data(), payload->size(), unsigned(std::max(2.0, timeout) * 1000));
    return true;
}

int32_t XLinkConnection::nextRequestId() {
    std::lock_guard guard(state_);
    int32_t id = nextId_;
    nextId_ = nextId_ == INT32_MAX ? 0x0B : nextId_ + 1;
    return id;
}

uint32_t XLinkConnection::freeId(const std::map<std::string, uint32_t> &streams) {
    for (uint32_t id = 0; id < 32; id++)
        if (std::none_of(streams.begin(), streams.end(), [&](auto &s) { return s.second == id; })) return id;
    return 31;
}

void XLinkConnection::markDead() {
    std::function<void()> notify;
    {
        std::lock_guard guard(state_);
        if (alive_) notify = onDeath_;
        alive_ = false;
    }
    changed_.notify_all();
    if (notify) notify();
}

// MARK: - Reader thread

void XLinkConnection::readLoop() {
    uint8_t headerBytes[Header::length];
    Bytes payload(1 << 20);
    int failures = 0;
    while (true) {
        { std::lock_guard guard(state_); if (!running_) return; }
        // Block until the device sends something; close() aborts the read.
        size_t n = 0;
        switch (pipe_->read(headerBytes, Header::length, 0, n)) {
        case USBPipe::Read::aborted: return;
        case USBPipe::Read::timeout: continue;
        case USBPipe::Read::failed:
            if (++failures > 5) { markDead(); return; }
            continue;
        case USBPipe::Read::data:
            failures = 0;
            break;
        }
        if (n != Header::length) continue;   // zero-length packet
        Header h = Header::decode(headerBytes);
        // Anything that isn't a plausible header means we've lost sync with the device.
        // Only writes carry a payload; other events can have stale size fields.
        if (h.type < 0 || h.type > 14 || h.type == 7 || (h.type == writeReq && h.size > maxPayload)) { markDead(); return; }

        Bytes body;
        if (h.type == writeReq && h.size > 0) {
            if (payload.size() < h.size) payload.resize(h.size);
            size_t got = 0;
            while (got < h.size) {
                size_t m = 0;
                if (pipe_->read(payload.data() + got, h.size - got, 3000, m) != USBPipe::Read::data) { markDead(); return; }
                got += m;
            }
            body.assign(payload.begin(), payload.begin() + got);
        }
        handle(h, std::move(body));
    }
}

void XLinkConnection::handle(const Header &h, Bytes body) {
    if (h.type >= writeResp) {
        std::lock_guard guard(state_);
        // Keep only responses someone is waiting for (not e.g. READ_REL_RESP to our releases).
        if (outstanding_.count(h.id)) {
            responses_[h.id] = h;
            changed_.notify_all();
        }
        return;
    }
    Header reply;
    reply.id = h.id;
    reply.type = h.type + 8;
    switch (h.type) {
    case writeReq: {
        reply.streamId = h.streamId;
        reply.size = h.size;
        if (!send(reply)) { markDead(); return; }
        deviceWrote(h.streamId, std::move(body));
        Header release;
        release.id = nextRequestId();
        release.type = readRelReq;
        release.streamId = h.streamId;
        release.size = h.size;
        send(release);
        break;
    }
    case createStreamReq: {
        uint32_t id;
        {
            std::lock_guard guard(state_);
            auto it = streams_.find(h.name);
            id = it != streams_.end() ? it->second : freeId(streams_);
            streams_[h.name] = id;
        }
        reply.name = h.name;
        reply.size = h.size;
        reply.streamId = id;
        send(reply);
        if (onLog_) onLog_("xlink: camera opened " + h.name + ", given stream " + std::to_string(id) + " (it asked for " + std::to_string(h.streamId) + ")");
        break;
    }
    case closeStreamReq:
        reply.streamId = h.streamId;
        send(reply);
        break;
    case readRelReq: case pingReq: case resetReq:
        send(reply);
        break;
    default:
        break;
    }
}

void XLinkConnection::deviceWrote(uint32_t stream, Bytes data) {
    std::string name;
    bool queued;
    {
        std::lock_guard guard(state_);
        for (auto &[n, id] : streams_) if (id == stream) { name = n; break; }
        queued = hostStreams_.count(stream);
    }
    if (name == "__timesync") {
        // Answer with our steady clock as {int64 sec, int64 nsec}.
        timespec ts{};
        clock_gettime(CLOCK_MONOTONIC, &ts);
        Bytes reply(16);
        int64_t sec = ts.tv_sec, nsec = ts.tv_nsec;
        for (int i = 0; i < 8; i++) { reply[i] = uint8_t(uint64_t(sec) >> (8 * i)); reply[8 + i] = uint8_t(uint64_t(nsec) >> (8 * i)); }
        Header h;
        h.id = nextRequestId();
        h.type = writeReq;
        h.streamId = stream;
        h.size = uint32_t(reply.size());
        send(h, &reply);
    } else if (name == "__log") {
        if (!onLog_) return;
        std::string text, word;
        for (uint8_t c : data) {
            if (c > 32 && c < 127) { word += char(c); continue; }
            if (!word.empty()) { text += (text.empty() ? "" : " ") + word; word.clear(); }
        }
        if (!word.empty()) text += (text.empty() ? "" : " ") + word;
        onLog_(text);
    } else {
        if (!queued) return;   // nobody will read it; already acknowledged and released
        std::lock_guard guard(state_);
        packets_[stream].push_back(std::move(data));
        changed_.notify_all();
    }
}
