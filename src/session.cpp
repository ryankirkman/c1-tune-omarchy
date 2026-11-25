#include "session.h"

#include <algorithm>
#include <cerrno>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <unistd.h>

#include "firmware.h"
#include "log.h"

namespace {
std::chrono::steady_clock::duration seconds(double s) {
    return std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(s));
}
void sleepFor(double s) { usleep(useconds_t(s * 1e6)); }

std::string printable(const Bytes &bytes, size_t from) {
    std::string out;
    for (size_t i = from; i < bytes.size(); i++) if (bytes[i] >= 32 && bytes[i] < 127) out += char(bytes[i]);
    return out;
}

// bcdDevice of the standalone app in the camera's flash (ours, Luxonis's build, is 0320).
const char *standaloneVersion = "0200";

const char *udevHint = "No USB access to the camera. Install the udev rule (see the README), then replug or press Retry.";
}

Session::Session(CameraSettings settings, std::function<void(Status)> onStatus, bool verbose)
    : onStatus_(std::move(onStatus)), verbose_(verbose), settings_(std::move(settings)) {
    wakeFd_ = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    inotifyFd_ = inotify_init1(IN_CLOEXEC | IN_NONBLOCK);
}

Session::~Session() {
    { std::lock_guard guard(queueLock_); stopping_ = true; }
    uint64_t one = 1;
    (void)!write(wakeFd_, &one, sizeof one);
    if (thread_.joinable()) thread_.join();
    bool release = releaseOnExit_ && link_ != nullptr;   // (the destructor has no need to track ghosts)
    link_.reset();
    // Quitting hands the camera back: reboot it, and with nobody talking to its bootloader
    // it starts the factory webcam app from flash. (Left alone, the firmware only resets
    // itself if no app has streamed video since it booted; otherwise it runs on unattended.)
    if (release) {
        auto old = USB::find(OpalPID::booted);
        USB::rebootToBootloader();
        // Nobody will be here to clear a leftover entry later, so see the camera into its
        // bootloader and clear it now if the kernel has kept it (see rebootBooted).
        if (old && monitor_.wait(OpalPID::bootloader, 3)) {
            auto still = USB::find(OpalPID::booted);
            if (still && still->node == old->node) USB::resetDevice(old->node);
        }
    }
    close(wakeFd_);
    close(inotifyFd_);
}

void Session::start() {
    thread_ = std::thread([this] { run(); });
}

void Session::send(const std::vector<CameraSettings::Group> &groups, const CameraSettings &settings) {
    {
        std::lock_guard guard(pendingLock_);
        for (auto group : groups) pending_[group] = settings.control(group);
    }
    post([this, settings] {
        settings_ = settings;
        flush();
    });
}

void Session::poke() {
    post([this] {
        failures_ = 0;
        retryAfter_ = {};
        tick();
    });
}

void Session::restart(const CameraSettings &settings) {
    post([this, settings] {
        settings_ = settings;
        { std::lock_guard guard(pendingLock_); pending_.clear(); }
        if (!link_) return;
        report({Status::starting, "Restarting camera…"});
        dropLink();
        // Straight to the bootloader (≈5 s to a fresh pipeline); an XLink reset would wait
        // for the standalone app to boot first (≈14 s).
        rebootBooted();
        retryAfter_ = Clock::now() + seconds(0.5);
        // The camera is off the bus for a moment between boot stages; that isn't "unplugged".
        expectedBack_ = Clock::now() + seconds(6);
        post([this] { tick(); }, 0.5);
        post([this] { tick(); }, 6.1);
    });
}

void Session::releaseCamera() {
    std::lock_guard guard(queueLock_);
    releaseOnExit_ = true;
}

// MARK: - Thread

void Session::post(std::function<void()> task, double delaySeconds) {
    {
        std::lock_guard guard(queueLock_);
        queue_.emplace(Clock::now() + seconds(delaySeconds), std::move(task));
    }
    uint64_t one = 1;
    (void)!write(wakeFd_, &one, sizeof one);
}

void Session::run() {
    watchVideoNodes();
    tick();
    while (true) {
        // Run what's due, then sleep until the next deadline or event.
        int timeout = -1;
        while (true) {
            std::function<void()> task;
            {
                std::lock_guard guard(queueLock_);
                if (stopping_) return;
                if (queue_.empty()) break;
                auto due = queue_.begin()->first;
                auto now = Clock::now();
                if (due > now) {
                    timeout = int(std::chrono::duration_cast<std::chrono::milliseconds>(due - now).count()) + 1;
                    break;
                }
                task = std::move(queue_.begin()->second);
                queue_.erase(queue_.begin());
            }
            task();
        }
        pollfd fds[3] = {{wakeFd_, POLLIN, 0}, {monitor_.fd(), POLLIN, 0}, {inotifyFd_, POLLIN, 0}};
        if (poll(fds, 3, timeout) < 0 && errno != EINTR) { sleepFor(1); continue; }
        if (fds[0].revents) { uint64_t count; (void)!read(wakeFd_, &count, sizeof count); }
        if (fds[1].revents) {
            auto changes = monitor_.drain();
            if (changes.video) watchVideoNodes();
            if (changes.usb) tick();
        }
        if (fds[2].revents) {
            char events[4096];
            bool opened = false;
            while (read(inotifyFd_, events, sizeof events) > 0) opened = true;
            if (opened) streamStarted();
        }
    }
}

/// An app opening the webcam's device node is the first sign it's about to stream, and
/// inotify reports that without polling. (PipeWire and browsers both open it on demand.)
void Session::watchVideoNodes() {
    for (int watch : videoWatches_) inotify_rm_watch(inotifyFd_, watch);
    videoWatches_.clear();
    for (auto &node : USB::videoNodes()) {
        int watch = inotify_add_watch(inotifyFd_, node.c_str(), IN_OPEN);
        if (watch >= 0) videoWatches_.push_back(watch);
    }
}

/// An app just opened the webcam or started its stream. The camera drops control messages
/// while its sensor is idle, so send every setting again once it's running.
void Session::streamStarted() {
    auto now = Clock::now();
    if (now - lastOpen_ < seconds(1)) return;   // the open and the stream start of one app, or several opens
    lastOpen_ = now;
    for (double delay : {0.8, 2.5}) {
        // Meant for the pipeline that is running now: after a restart they would only pile
        // onto the new one's first moments.
        post([this, boot = boot_] {
            if (boot != boot_) return;
            {
                std::lock_guard guard(pendingLock_);
                for (int g = 0; g < CameraSettings::groupCount; g++) pending_[CameraSettings::Group(g)] = settings_.control(CameraSettings::Group(g));
            }
            flush();
        }, delay);
    }
}

void Session::flush() {
    if (!link_ || !controlStream_) return;
    std::vector<CameraControl> batch;
    {
        std::lock_guard guard(pendingLock_);
        for (auto &[group, control] : pending_) batch.push_back(control);
        pending_.clear();
    }
    if (batch.empty()) return;
    // Each group sets its own commands, so they combine without conflicts.
    CameraControl merged;
    for (auto &c : batch) merged.absorb(c);
    if (!link_->write(*controlStream_, merged.encoded(), 2)) logLine("control write failed");
    else if (verbose_) logLine("control sent (mask %llx)", (unsigned long long)merged.cmdMask);
}

void Session::dropLink() {
    boot_++;
    if (link_) link_->close();
    link_.reset();
    controlStream_.reset();
}

void Session::tick() {
    if (link_) {
        if (link_->isAlive()) return;
        logLine("camera link lost");
        dropLink();
    }
    bool present = USB::isPresent(OpalPID::booted) || USB::isPresent(OpalPID::bootloader) || USB::isPresent(OpalPID::rom);
    if (present && !wasPresent_) { failures_ = 0; retryAfter_ = {}; }   // plugged in: start over
    wasPresent_ = present;
    auto now = Clock::now();
    if (!present) {
        if (now >= retryAfter_ && now >= expectedBack_) report({Status::notConnected, "Not connected"});
        return;
    }
    if (failures_ >= maxFailures) return;
    if (now < retryAfter_) {   // also covers the moment between boot stages during a restart
        post([this] { tick(); }, std::chrono::duration<double>(retryAfter_ - now).count());
        return;
    }
    // Without the udev rule nothing below can work; say so instead of retrying.
    for (auto pid : {OpalPID::booted, OpalPID::bootloader, OpalPID::rom}) {
        if (USB::lacksPermission(pid)) { report({Status::failed, udevHint}); return; }
    }
    try {
        auto started = Clock::now();
        connect();
        failures_ = 0;
        logLine("camera ready (%.2f s)", std::chrono::duration<double>(Clock::now() - started).count());
        report({Status::ready, "Ready"});
        {
            std::lock_guard guard(pendingLock_);
            for (int g = 0; g < CameraSettings::groupCount; g++) {
                auto group = CameraSettings::Group(g);
                if (!pending_.count(group)) pending_[group] = settings_.control(group);
            }
        }
        flush();
    } catch (const Busy &) {
        retryAfter_ = Clock::now() + seconds(5);
        report({Status::busy, "In use by another app"});
        post([this] { tick(); }, 5);
    } catch (const StartError &error) {
        logLine("start failed: %s", error.message.c_str());
        dropLink();
        failures_++;
        static const double backoff[] = {3, 10, 30, 60};
        double wait = backoff[std::min(failures_ - 1, 3)];
        retryAfter_ = Clock::now() + seconds(wait);
        if (failures_ >= maxFailures) report({Status::failed, error.message});
        else {
            report({Status::starting, "Retrying…"});
            post([this] { tick(); }, wait);
        }
    }
}

void Session::report(Status status) {
    if (lastStatus_ && status == *lastStatus_) return;
    lastStatus_ = status;
    if (verbose_) logLine("status: %s", status.message.c_str());
    if (onStatus_) onStatus_(status);
}

// MARK: - Connect or boot

/// Asks the running firmware to reboot, remembering which device that was. The kernel does
/// not always notice a USB 3 device leaving: its entry can stay listed for minutes while the
/// camera is really in its bootloader on the USB 2 side. Until it is replaced, that entry is
/// a ghost, and mistaking it for the freshly booted camera makes every later step fail.
void Session::rebootBooted() {
    if (auto device = USB::find(OpalPID::booted)) {
        ghost_ = device->node;
        ghostUntil_ = Clock::now() + seconds(30);
        ghostReset_ = false;
    }
    // A request that fails outright means the entry was already dead (a ghost from before
    // this app started, or a camera that has hung): have the kernel look at the port again.
    auto device = USB::find(OpalPID::booted);
    if (!USB::rebootToBootloader() && device) {
        logLine("the camera didn't take the reboot request; resetting its port");
        USB::resetDevice(device->node);
    }
}

/// A running camera that isn't the ghost of one we rebooted. (A re-enumerated device gets a
/// new device number, so its node differs.)
bool Session::bootedPresent() {
    auto device = USB::find(OpalPID::booted);
    if (!device) return false;
    if (Clock::now() >= ghostUntil_) ghost_.clear();
    // A camera is one device: if it is also showing as bootloader or boot ROM, this entry is
    // a ghost. Resetting its port makes the kernel notice and drop it (a real device can't be
    // in two stages, so this never touches a working camera).
    if (USB::isPresent(OpalPID::bootloader) || USB::isPresent(OpalPID::rom)) {
        if (ghost_ != device->node) ghostReset_ = false;
        ghost_ = device->node;
        ghostUntil_ = Clock::now() + seconds(30);
        if (!ghostReset_) {
            ghostReset_ = true;
            logLine("the kernel still lists the camera's old USB entry; clearing it");
            USB::resetDevice(device->node);
        }
        return false;
    }
    return device->node != ghost_;
}

template <typename Open> auto Session::retrying(const char *message, int attempts, Open open) {
    // Retries `open` every 250 ms; the device takes a moment to register its interfaces.
    for (int i = 0; i < attempts; i++) {
        if (auto value = open()) return value;
        sleepFor(0.25);
    }
    throw StartError{message};
}

void Session::connect(bool secondTry) {
    auto began = Clock::now();
    auto step = [&](const char *what) {
        if (verbose_) logLine("+%.2f s: %s", std::chrono::duration<double>(Clock::now() - began).count(), what);
    };
    XLinkConnection::Log logDevice = [this](const std::string &text) {
        if (verbose_) logLine(text.rfind("xlink: ", 0) == 0 ? "%.1500s" : "device: %.1500s", text.c_str());
        // The firmware prints this the moment a host starts the video stream: the exact
        // signal the settings need re-sending on (called on the link's reader thread).
        if (text.find("UvcDevice_Start") != std::string::npos) post([this] { streamStarted(); });
    };
    // Called on a link thread when the link drops on its own: wake the session to notice.
    auto onDeath = [this] { post([this] { tick(); }); };
    using Error = XLinkConnection::ConnectError;

    // Our firmware answers XLink; Opal's standalone app doesn't accept the first write, but
    // only says so by letting it time out after 2 s. The two report different device
    // versions over USB, so when it clearly isn't our firmware, skip asking.
    if (bootedPresent() && USB::deviceVersion(OpalPID::booted) != standaloneVersion) {
        report({Status::starting, "Connecting to the camera…"});   // instant when it answers; seconds when it has hung
        Error error;
        auto existing = XLinkConnection::connect(OpalPID::booted, error, 3, logDevice, onDeath);
        if (!existing && error == Error::noAnswer) existing = XLinkConnection::connect(OpalPID::booted, error, 3, logDevice, onDeath);
        if (existing) {
            try {
                attach(std::move(existing), false);
                return;
            } catch (const StartError &e) {
                logLine("reattach failed, rebooting camera: %s", e.message.c_str());
                dropLink();
            }
        } else if (error == Error::busy) {
            throw Busy{};
        } else if (error == Error::permission) {
            throw StartError{udevHint};
        }
    }

    report({Status::starting, "Starting camera…"});
    std::string firmwareError;
    Bytes firmware = Firmware::load(firmwareError);
    if (firmware.empty()) throw StartError{firmwareError};
    BootImage::patchFrameSizes(firmware);
    step("firmware loaded");

    // Either we ask it to reboot now, or we already have (a restart) and it is on its way.
    bool rebooting = !ghost_.empty() && Clock::now() < ghostUntil_;
    if (bootedPresent()) {
        rebootBooted();
        step("asked the camera to reboot to its bootloader");
        rebooting = true;
    }
    if (rebooting) {
        // Wait for the camera to show up in its next stage. Checking bootedPresent() as it
        // does also clears the old entry if the kernel has left it behind.
        bool moved = monitor_.waitUntil([this] {
            bool other = USB::isPresent(OpalPID::bootloader) || USB::isPresent(OpalPID::rom);
            return bootedPresent() || other;
        }, 6);
        if (!moved) throw StartError{"The camera isn't responding. Unplug it and plug it back in."};
    }
    if (USB::isPresent(OpalPID::bootloader)) {
        step("bootloader present");
        auto bootloader = retrying("Couldn't talk to the camera's bootloader.", 12, [] {
            Error error;
            return XLinkConnection::connect(OpalPID::bootloader, error);
        });
        step("bootloader answered");
        auto watchdog = bootloader->open("__watchdog", 64);
        if (watchdog) bootloader->write(*watchdog, {0, 0, 0, 0}, 1);   // keepalive
        auto stream = bootloader->open("__bootloader", 5 << 20);
        bool asked = stream && bootloader->write(*stream, {0, 0, 0, 0}, 2);  // USB_ROM_BOOT
        if (verbose_) logLine("bootloader: watchdog %s, USB_ROM_BOOT %s", watchdog ? "open" : "refused", asked ? "sent" : stream ? "not acknowledged" : "stream refused");
        bool factory = false;
        bool moved = monitor_.waitUntil([&] { return USB::isPresent(OpalPID::rom) || (factory = bootedPresent()); }, 6);
        bootloader->close();
        if (moved && factory && !secondTry) {
            // The bootloader had already given up waiting for a host and was on its way to
            // the factory app when we asked (the app relaunched a few seconds after a quit).
            // That app is up now, so go round once more from the reboot request.
            step("the bootloader started the factory app instead; starting over");
            return connect(true);
        }
        if (!moved || factory) throw StartError{"The camera didn't enter USB boot mode."};
    }
    if (USB::isPresent(OpalPID::rom)) {
        step("boot ROM present");
        auto rom = retrying("Couldn't open the camera in USB boot mode.", 12, [] {
            USBPipe::OpenError error;
            return USBPipe::open(OpalPID::rom, error);
        });
        // Default watchdog (fed from our keepalive thread); 0 makes the firmware reset when video starts.
        upload(BootImage::image(firmware, BootImage::boardConfig(std::nullopt, true)), *rom);
        rom->close();
        step("firmware uploaded");
        if (!monitor_.waitUntil([this] { return bootedPresent(); }, 10))
            throw StartError{"The camera didn't come back after loading firmware."};
    }
    auto connection = retrying("Couldn't connect to the camera after loading firmware.", 20, [&] {
        Error error;
        if (!bootedPresent()) return std::unique_ptr<XLinkConnection>();   // only a ghost so far
        return XLinkConnection::connect(OpalPID::booted, error, 3, logDevice, onDeath);
    });
    step("firmware answered");
    attach(std::move(connection), true);
}

void Session::upload(const Bytes &image, USBPipe &pipe) {
    const size_t chunk = 1 << 20;
    for (size_t offset = 0; offset < image.size();) {
        size_t n = std::min(chunk, image.size() - offset);
        if (!pipe.write(image.data() + offset, n, 2000)) throw StartError{"Firmware upload failed."};
        offset += n;
    }
    if (image.size() % 512 == 0) pipe.write(nullptr, 0, 2000);   // zero-length packet ends the transfer
}

void Session::attach(std::unique_ptr<XLinkConnection> connection, bool freshBoot) {
    link_ = std::move(connection);
    if (auto watchdog = link_->open("__watchdog", 128)) link_->feedWatchdog(*watchdog);
    auto rpc = link_->open("__rpc_main", 5 << 20);
    auto control = rpc ? link_->open("control", 5 << 20) : std::nullopt;
    if (!rpc || !control) throw StartError{"Couldn't open the camera's control channels."};
    rpcStream_ = *rpc;

    if (!freshBoot) {
        Bytes running = call("isPipelineRunning");
        if (running.empty() || running.back() != 0xC3) throw StartError{"The camera's pipeline isn't running."};
        controlStream_ = control;
        return;
    }

    report({Status::starting, "Configuring camera…"});
    if (settings_.microphone) waitForAudioProbe();
    Pipeline::Camera camera = settings_.pipelineCamera();
    Bytes sensors;
    try { sensors = call("getCameraSensorNames"); } catch (const StartError &) {}
    std::string source;
    auto tuning = Firmware::tuningBlob(sensors, settings_.tuningFile, source);
    if (tuning) logLine("tuning: %s", source.c_str());
    else logLine("tuning: the firmware's own%s%s", source.empty() ? "" : " (", source.empty() ? "" : (source + ")").c_str());
    Bytes schema = Pipeline::setPipelineSchemaRequest(camera, settings_.pipelineMic(), tuning ? Assets::tuningUri : "",
                                                      tuning ? std::optional<int64_t>(tuning->size()) : std::nullopt);
    call("setPipelineSchema", {}, &schema);
    if (tuning) uploadAsset(*tuning);
    Bytes built = call("buildPipeline");
    // Reply is [1, 2, 1, [ok, message]]: 94 01 02 01 92 C3|C2 <string>.
    if (!(built.size() >= 6 && built[4] == 0x92 && built[5] == 0xC3))
        throw StartError{"The camera rejected the pipeline: " + printable(built, 6)};
    call("startPipeline");
    controlStream_ = control;
}

/// The moment the camera's sound card appears, PipeWire opens and closes its capture device
/// dozens of times within a tenth of a second to probe it. If the pipeline's microphone
/// node starts in the middle of that, the firmware hangs for good (no video, no USB, until
/// it is unplugged). macOS doesn't probe this way, so this wait is Linux's alone: hold the
/// pipeline until the capture device has been left alone for 400 ms. inotify reports the
/// opens and closes; nothing is polled.
void Session::waitForAudioProbe() {
    int fd = inotify_init1(IN_CLOEXEC | IN_NONBLOCK);
    if (fd < 0 || inotify_add_watch(fd, "/dev/snd", IN_OPEN | IN_CLOSE | IN_CREATE) < 0) {
        if (fd >= 0) close(fd);
        sleepFor(1.5);   // can't watch: wait out a typical probe instead
        return;
    }
    // With PipeWire in the session the probe is certain to come, so if it hasn't started yet
    // (PipeWire still starting at login) wait for it rather than start underneath it.
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    bool pipewire = runtime && access((std::string(runtime) + "/pipewire-0").c_str(), F_OK) == 0;
    auto started = Clock::now();
    auto quietSince = started;
    int events = 0;
    while (true) {
        auto now = Clock::now();
        bool quiet = now - quietSince >= seconds(0.4);
        if (now - started >= seconds(8) || (quiet && (events > 0 || !pipewire || now - started >= seconds(5)))) break;
        pollfd p{fd, POLLIN, 0};
        int left = quiet ? 250 : int(std::chrono::duration_cast<std::chrono::milliseconds>(seconds(0.4) - (now - quietSince)).count()) + 1;
        if (poll(&p, 1, left) <= 0) continue;
        auto ours = USB::audioCaptureNodes();
        alignas(inotify_event) char buf[4096];
        ssize_t n;
        while ((n = read(fd, buf, sizeof buf)) > 0) {
            for (char *at = buf; at < buf + n;) {
                auto *event = reinterpret_cast<inotify_event *>(at);
                at += sizeof(inotify_event) + event->len;
                if (event->len && std::find(ours.begin(), ours.end(), std::string(event->name)) != ours.end()) {
                    quietSince = Clock::now();
                    events++;
                }
            }
        }
    }
    close(fd);
    if (verbose_) logLine("sound card quiet after %.2f s (%d opens and closes)",
                          std::chrono::duration<double>(Clock::now() - started).count(), events);
}

/// setAssets → memAlloc → readFromXLink (with the blob on its stream) → setAssetStorage.
void Session::uploadAsset(const Bytes &blob) {
    if (!link_) return;
    int64_t size = int64_t(blob.size());
    call("setAssets", {Assets::tuning(size)});
    Bytes alloc = call("memAlloc", {MsgPack::integer(size)});
    auto handle = RPC::trailingUInt(alloc);
    if (!handle || *handle == 0) throw StartError{"The camera didn't allocate memory for the tuning file."};
    // Open the data stream before asking the camera to read from it.
    auto storage = link_->open("__stream_asset_storage", 5 << 20);
    if (!storage) throw StartError{"Tuning upload failed."};
    Bytes request = RPC::request("readFromXLink", {MsgPack::string("__stream_asset_storage"), MsgPack::integer(int64_t(*handle)),
                                                   MsgPack::integer(size)});
    if (!link_->write(rpcStream_, request) || !link_->write(*storage, blob, 10)) throw StartError{"Tuning upload failed."};
    auto reply = link_->read(rpcStream_, 15);
    if (!reply || !RPC::succeeded(*reply)) throw StartError{"Tuning upload wasn't accepted."};
    call("setAssetStorage", {MsgPack::integer(int64_t(*handle)), MsgPack::integer(size)});
}

Bytes Session::call(const std::string &name, const std::vector<MsgPack> &args, const Bytes *raw) {
    if (!link_) throw StartError{"Camera disconnected."};
    std::optional<Bytes> reply;
    if (!link_->write(rpcStream_, raw ? *raw : RPC::request(name, args)) || !(reply = link_->read(rpcStream_, 15)))
        throw StartError{"The camera didn't answer " + name + "."};
    if (!RPC::succeeded(*reply)) throw StartError{name + " failed: " + printable(*reply, 4)};
    return *reply;
}
