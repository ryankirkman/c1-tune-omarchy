// A minimal XLink host: the link protocol DepthAI devices speak over the vendor bulk
// interface. Every message is an 84-byte header, plus a payload for writes. A reader
// thread answers everything the device initiates (stream creation, writes, releases,
// pings, timesync); `open`, `write` and `read` are blocking and run off the main thread.
//
// Ported from C1 Tune for macOS (Sources/XLink.swift): same headers, ids, replies and order.
#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>

#include "depthai.h"
#include "usb.h"

class XLinkConnection {
public:
    enum class ConnectError {
        none,
        notFound,
        busy,            // another app holds the camera
        permission,      // the udev rule isn't installed
        notListening,    // the device didn't accept our first write (e.g. Opal's standalone app)
        noAnswer,        // it accepted the PING but never replied
    };
    using Log = std::function<void(const std::string &)>;

    /// Opens interface 0 of the device in this boot stage and handshakes with a PING.
    /// `onDeath` is called (from a link thread) when the link drops on its own.
    static std::unique_ptr<XLinkConnection> connect(OpalPID pid, ConnectError &error, double pingTimeout = 3,
                                                    Log onLog = nullptr, std::function<void()> onDeath = nullptr);
    ~XLinkConnection();

    bool isAlive();

    /// Stops the threads and releases the interface. Never sends a reset, so the
    /// device keeps running whatever it's running.
    void close();

    /// Writes the 4-byte keepalive on `stream` every 500 ms on its own thread until the link
    /// closes. Separate from everything else so slow RPCs can never starve it.
    void feedWatchdog(uint32_t stream);

    /// Creates (or joins) a named stream; returns the id the device uses for it.
    std::optional<uint32_t> open(const std::string &name, uint32_t size);

    /// Writes one packet and waits for the device to acknowledge it.
    bool write(uint32_t stream, const Bytes &data, double timeout = 3);

    /// Next packet the device wrote on `stream`, or nullopt on timeout or a dropped link.
    std::optional<Bytes> read(uint32_t stream, double timeout);

private:
    enum Event : int32_t {
        writeReq = 0, readReq, readRelReq, createStreamReq, closeStreamReq, pingReq, resetReq,
        writeResp = 8, readResp, readRelResp, createStreamResp, closeStreamResp, pingResp, resetResp
    };

    struct Header {
        int32_t id = 0;
        int32_t type = 0;
        std::string name;
        uint32_t streamId = 0;
        uint32_t size = 0;
        uint32_t flags = 1;     // ack

        static constexpr size_t length = 84;
        void encode(uint8_t *out) const;
        static Header decode(const uint8_t *in);
    };

    enum class Outcome { response, sendFailed, timedOut };

    explicit XLinkConnection(std::unique_ptr<USBPipe> pipe, Log onLog);
    Outcome exchange(Header header, const Bytes *payload, double timeout, Header *response);
    bool send(const Header &h, const Bytes *payload = nullptr, double timeout = 3);
    int32_t nextRequestId();
    static uint32_t freeId(const std::map<std::string, uint32_t> &streams);
    void markDead();
    void readLoop();
    void handle(const Header &h, Bytes body);
    void deviceWrote(uint32_t stream, Bytes data);

    std::unique_ptr<USBPipe> pipe_;
    Log onLog_;
    std::function<void()> onDeath_;
    std::mutex sendLock_;
    std::mutex state_;                       // guards everything below
    std::condition_variable changed_;
    std::set<int32_t> outstanding_;          // ids of our requests still waiting for a response
    std::map<int32_t, Header> responses_;
    int32_t nextId_ = 0x0B;
    std::map<std::string, uint32_t> streams_;
    std::set<uint32_t> hostStreams_;         // streams we opened; only these queue packets
    std::map<uint32_t, std::deque<Bytes>> packets_;
    bool running_ = true;
    bool alive_ = true;
    bool closed_ = false;
    std::thread reader_;
    std::thread watchdog_;
};
