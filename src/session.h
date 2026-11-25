// Keeps the Opal C1 running our pipeline and delivers control messages to it.
//
// Out of the box the camera runs a standalone app that ignores control requests. When it
// shows up like that, the session reboots it into its bootloader, has that drop to the
// chip's USB boot ROM, uploads DepthAI firmware with UVC on and a 1080p-only webcam format,
// then starts a pipeline: camera → webcam output, plus a "control" input. A keepalive thread
// feeds the camera's watchdog; if this app quits, the camera resets to its standalone webcam
// after about 1.5 s.
//
// Ported from C1 Tune for macOS (Sources/OpalSession.swift): the same steps, in the same
// order, with the same timeouts. What differs is how it waits. All work happens on one
// thread that sleeps in poll() until udev reports a USB change, an app opens the webcam,
// the link drops, or a deadline it set itself arrives; nothing runs on a timer.
#pragma once

#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "settings.h"
#include "usb.h"
#include "xlink.h"

class Session {
public:
    struct Status {
        enum Kind { notConnected, starting, ready, busy, failed } kind = notConnected;
        std::string message;
        bool operator==(const Status &) const = default;
    };

    /// `onStatus` is called from the session's thread.
    Session(CameraSettings settings, std::function<void(Status)> onStatus, bool verbose);
    ~Session();

    /// Starts watching for the camera.
    void start();

    /// Sends commands for these groups as soon as the camera is ready, and remembers the
    /// settings for the next start. Rapid calls keep only the newest message per group.
    void send(const std::vector<CameraSettings::Group> &groups, const CameraSettings &settings);

    /// Re-checks the camera now, e.g. when the user taps Retry.
    void poke();

    /// Restarts the camera so the pipeline is rebuilt from these settings (zoom, framing,
    /// flip, microphone, or returning sharpness and denoise to the tuning's defaults).
    void restart(const CameraSettings &settings);

    /// Makes the destructor send the camera back to its factory webcam mode (for a
    /// deliberate quit, as opposed to the app being killed).
    void releaseCamera();

private:
    using Clock = std::chrono::steady_clock;
    struct StartError { std::string message; };
    struct Busy {};

    void run();
    void post(std::function<void()> task, double delaySeconds = 0);
    void flush();
    void tick();
    void report(Status status);
    void streamStarted();
    void watchVideoNodes();
    void dropLink();

    void connect(bool secondTry = false);
    bool bootedPresent();
    void rebootBooted();
    template <typename Open> auto retrying(const char *message, int attempts, Open open);
    void upload(const Bytes &image, USBPipe &pipe);
    void attach(std::unique_ptr<XLinkConnection> connection, bool freshBoot);
    void waitForAudioProbe();
    void uploadAsset(const Bytes &blob);
    Bytes call(const std::string &name, const std::vector<MsgPack> &args = {}, const Bytes *raw = nullptr);

    std::function<void(Status)> onStatus_;
    bool verbose_;
    std::thread thread_;
    int wakeFd_ = -1;
    int inotifyFd_ = -1;
    USBMonitor monitor_;

    std::mutex queueLock_;
    std::multimap<Clock::time_point, std::function<void()>> queue_;   // guarded by queueLock_
    bool stopping_ = false;                                          // guarded by queueLock_
    bool releaseOnExit_ = false;                                     // guarded by queueLock_

    // Everything below is used on the session thread only.
    std::unique_ptr<XLinkConnection> link_;
    uint32_t rpcStream_ = 0;
    std::optional<uint32_t> controlStream_;
    CameraSettings settings_;
    std::optional<Status> lastStatus_;
    int failures_ = 0;
    Clock::time_point retryAfter_{};
    Clock::time_point expectedBack_{};
    std::string ghost_;                 // device node of a booted camera we have told to reboot
    Clock::time_point ghostUntil_{};
    bool ghostReset_ = false;
    bool wasPresent_ = false;
    Clock::time_point lastOpen_{};
    unsigned boot_ = 0;   // counts links, so work queued for one doesn't reach the next
    std::vector<int> videoWatches_;

    std::mutex pendingLock_;
    std::map<CameraSettings::Group, CameraControl> pending_;   // guarded by pendingLock_

    static constexpr int maxFailures = 4;
};
