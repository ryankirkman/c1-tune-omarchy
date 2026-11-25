// USB access through the kernel's usbfs (/dev/bus/usb), with no library in between, and
// device arrival and removal from udev's netlink broadcasts instead of polling.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

/// USB product IDs the Myriad X inside the Opal C1 shows up as, by boot stage.
enum class OpalPID : uint16_t {
    booted = 0xF63B,        // running firmware (Opal's standalone app, or ours)
    bootloader = 0xF63C,
    rom = 0x2485,           // chip's USB boot ROM, waiting for firmware
};

namespace USB {
inline const uint16_t vendorID = 0x03E7;

struct Device {
    std::string sysfs;      // /sys/bus/usb/devices/4-2.1.1
    std::string node;       // /dev/bus/usb/004/013
};

std::optional<Device> find(OpalPID pid);
inline bool isPresent(OpalPID pid) { return find(pid).has_value(); }

/// True when the device is there but its node isn't writable: the udev rule is missing.
bool lacksPermission(OpalPID pid);

/// Sends a control request with no data stage. The device often drops off the bus
/// in response, so the result is informational only.
bool deviceRequest(OpalPID pid, uint8_t type, uint8_t request, uint16_t value, uint16_t index);

/// Asks the running firmware (Opal's standalone app or ours) to reboot into the bootloader.
inline void rebootToBootloader() { deviceRequest(OpalPID::booted, 0x00, 0xF5, 0x0DA1, 0); }

/// /dev/videoN nodes that belong to the booted camera.
std::vector<std::string> videoNodes();
}

/// udev's device events (the "udev" netlink group, sent once rules, and so permissions,
/// have been applied). `fd()` becomes readable when something happened.
class USBMonitor {
public:
    USBMonitor();
    ~USBMonitor();
    int fd() const { return fd_; }

    struct Changes { bool usb = false, video = false; };
    /// Reads everything queued and says whether any of it was about the camera.
    Changes drain();

    /// Blocks until a device with `pid` is present, without polling. False on timeout.
    bool wait(OpalPID pid, double timeoutSeconds);

private:
    int fd_ = -1;
};

/// Exclusive access to interface 0 (the vendor bulk interface) in one boot stage.
/// Reads and writes are blocking; callers keep them off the main thread.
class USBPipe {
public:
    enum class OpenError { none, notFound, busy, permission, failed };

    /// Opens interface 0, or tells why not: `busy` means another app holds the camera.
    static std::unique_ptr<USBPipe> open(OpalPID pid, OpenError &error);
    ~USBPipe();

    /// Releases the interface. Unblocks a pending read first.
    void close();
    /// Makes a blocking `read` return `aborted` so its thread can exit.
    void abortRead();

    /// Writes all of `data` (count 0 sends a zero-length packet); false on error or timeout.
    bool write(const void *data, size_t count, unsigned timeoutMs);

    enum class Read { data, timeout, aborted, failed };
    /// Reads up to `count` bytes into `buffer`, setting `got`. `timeoutMs` 0 waits until data
    /// arrives or `abortRead()`.
    Read read(void *buffer, size_t count, unsigned timeoutMs, size_t &got);

private:
    USBPipe() = default;
    int fd_ = -1;
    int abortFd_ = -1;
    uint8_t epOut_ = 0;
    uint8_t epIn_ = 0;      // 0 when the interface has no IN endpoint
    std::mutex lock_;
    bool open_ = true;      // guarded by lock_
};
