#include "usb.h"


#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <linux/netlink.h>
#include <linux/usbdevice_fs.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {
std::string readLine(const std::string &path) {
    char buf[256];
    int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return "";
    ssize_t n = ::read(fd, buf, sizeof buf - 1);
    ::close(fd);
    if (n <= 0) return "";
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == ' ')) n--;
    return std::string(buf, size_t(n));
}

bool matches(const std::string &sysfs, OpalPID pid) {
    char want[8];
    snprintf(want, sizeof want, "%04x", unsigned(pid));
    return readLine(sysfs + "/idVendor") == "03e7" && readLine(sysfs + "/idProduct") == want;
}
}

std::optional<USB::Device> USB::find(OpalPID pid) {
    const std::string root = "/sys/bus/usb/devices";
    DIR *dir = opendir(root.c_str());
    if (!dir) return {};
    std::optional<Device> found;
    while (dirent *e = readdir(dir)) {
        // Devices are "1-2.3"; interfaces ("1-2.3:1.0") and root hubs ("usb1") aren't the camera.
        if (e->d_name[0] == '.' || strchr(e->d_name, ':') || !strncmp(e->d_name, "usb", 3)) continue;
        std::string sysfs = root + "/" + e->d_name;
        if (!matches(sysfs, pid)) continue;
        int bus = atoi(readLine(sysfs + "/busnum").c_str()), dev = atoi(readLine(sysfs + "/devnum").c_str());
        if (bus <= 0 || dev <= 0) continue;
        char node[64];
        snprintf(node, sizeof node, "/dev/bus/usb/%03d/%03d", bus, dev);
        found = Device{sysfs, node};
        break;
    }
    closedir(dir);
    return found;
}

std::string USB::deviceVersion(OpalPID pid) {
    auto device = find(pid);
    return device ? readLine(device->sysfs + "/bcdDevice") : "";
}

bool USB::lacksPermission(OpalPID pid) {
    auto device = find(pid);
    return device && access(device->node.c_str(), R_OK | W_OK) != 0 && errno == EACCES;
}

bool USB::deviceRequest(OpalPID pid, uint8_t type, uint8_t request, uint16_t value, uint16_t index) {
    auto device = find(pid);
    if (!device) return false;
    int fd = ::open(device->node.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0) return false;
    usbdevfs_ctrltransfer transfer{};
    transfer.bRequestType = type;
    transfer.bRequest = request;
    transfer.wValue = value;
    transfer.wIndex = index;
    transfer.wLength = 0;
    transfer.timeout = 1000;
    int r = ioctl(fd, USBDEVFS_CONTROL, &transfer);
    ::close(fd);
    return r >= 0;
}

std::vector<std::string> USB::videoNodes() {
    std::vector<std::string> nodes;
    const std::string root = "/sys/class/video4linux";
    DIR *dir = opendir(root.c_str());
    if (!dir) return nodes;
    while (dirent *e = readdir(dir)) {
        if (strncmp(e->d_name, "video", 5)) continue;
        // video0/device is the USB interface; its parent is the USB device.
        if (matches(root + "/" + e->d_name + "/device/..", OpalPID::booted)) nodes.push_back(std::string("/dev/") + e->d_name);
    }
    closedir(dir);
    return nodes;
}

// MARK: - Monitor

USBMonitor::USBMonitor() {
    fd_ = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC | SOCK_NONBLOCK, NETLINK_KOBJECT_UEVENT);
    if (fd_ < 0) return;
    sockaddr_nl address{};
    address.nl_family = AF_NETLINK;
    address.nl_groups = 2;   // udev's rebroadcast, after rules have run (1 is the raw kernel events)
    if (bind(fd_, reinterpret_cast<sockaddr *>(&address), sizeof address) != 0) { ::close(fd_); fd_ = -1; }
}

USBMonitor::~USBMonitor() { if (fd_ >= 0) ::close(fd_); }

USBMonitor::Changes USBMonitor::drain() {
    Changes changes;
    char buf[8192];
    while (true) {
        sockaddr_nl from{};
        iovec iov{buf, sizeof buf};
        msghdr msg{};
        msg.msg_name = &from;
        msg.msg_namelen = sizeof from;
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        ssize_t n = recvmsg(fd_, &msg, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && errno == ENOBUFS) { changes.usb = changes.video = true; continue; }   // missed some: assume the worst
        if (n <= 0) break;
        // udev messages: "libudev\0", then a header whose third word is where the
        // KEY=VALUE\0 properties start. Only root (udevd) may send to this group.
        if (from.nl_pid == 0 || n < 40 || memcmp(buf, "libudev", 8) != 0) continue;
        uint32_t offset;
        memcpy(&offset, buf + 16, 4);
        if (offset >= size_t(n)) continue;
        bool usbDevice = false, v4l = false, ours = false;
        for (const char *p = buf + offset, *end = buf + n; p < end; p += strlen(p) + 1) {
            if (!memchr(p, 0, size_t(end - p))) break;
            if (!strcmp(p, "DEVTYPE=usb_device")) usbDevice = true;
            else if (!strcmp(p, "SUBSYSTEM=video4linux")) v4l = true;
            else if (!strncmp(p, "PRODUCT=3e7/", 12)) ours = true;
            else if (!strcmp(p, "ID_VENDOR_ID=03e7")) ours = true;
        }
        if (usbDevice && ours) changes.usb = true;
        if (v4l && ours) changes.video = true;
    }
    return changes;
}

bool USBMonitor::wait(OpalPID pid, double timeoutSeconds) {
    using clock = std::chrono::steady_clock;
    auto deadline = clock::now() + std::chrono::duration<double>(timeoutSeconds);
    while (true) {
        if (USB::isPresent(pid)) return true;
        auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - clock::now()).count();
        if (left <= 0) return false;
        if (fd_ < 0) { usleep(100000); continue; }   // no netlink: fall back to the original's 100 ms poll
        pollfd p{fd_, POLLIN, 0};
        if (poll(&p, 1, int(left)) > 0) drain();
    }
}

// MARK: - Pipe

std::unique_ptr<USBPipe> USBPipe::open(OpalPID pid, OpenError &error) {
    auto device = USB::find(pid);
    if (!device) { error = OpenError::notFound; return nullptr; }
    int fd = ::open(device->node.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        error = errno == EACCES ? OpenError::permission : errno == ENOENT || errno == ENODEV ? OpenError::notFound : OpenError::failed;
        return nullptr;
    }
    // The node reads as the device descriptor followed by its configuration descriptors.
    // Find interface 0's bulk endpoints.
    uint8_t d[4096];
    ssize_t n = ::read(fd, d, sizeof d);
    uint8_t out = 0, in = 0;
    int interface = -1, alternate = -1;
    for (ssize_t i = 18; n > 18 && i + 2 <= n && d[i] >= 2 && i + d[i] <= n; i += d[i]) {
        if (d[i + 1] == 4 && d[i] >= 9) { interface = d[i + 2]; alternate = d[i + 3]; }
        if (d[i + 1] == 5 && d[i] >= 7 && interface == 0 && alternate == 0 && (d[i + 3] & 3) == 2) {
            if (!(d[i + 2] & 0x80) && out == 0) out = d[i + 2];
            if ((d[i + 2] & 0x80) && in == 0) in = d[i + 2];
        }
        if (d[i + 1] == 2 && i > 18) break;   // a second configuration: only the first is used
    }
    if (out == 0) { ::close(fd); error = OpenError::failed; return nullptr; }
    unsigned number = 0;
    if (ioctl(fd, USBDEVFS_CLAIMINTERFACE, &number) != 0) {
        error = errno == EBUSY ? OpenError::busy : errno == ENODEV ? OpenError::notFound : OpenError::failed;
        ::close(fd);
        return nullptr;
    }
    std::unique_ptr<USBPipe> pipe(new USBPipe());
    pipe->fd_ = fd;
    pipe->abortFd_ = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    pipe->epOut_ = out;
    pipe->epIn_ = in;
    error = OpenError::none;
    return pipe;
}

USBPipe::~USBPipe() {
    close();
    if (abortFd_ >= 0) ::close(abortFd_);
}

void USBPipe::close() {
    std::lock_guard guard(lock_);
    if (!open_) return;
    open_ = false;
    uint64_t one = 1;
    if (abortFd_ >= 0) (void)!::write(abortFd_, &one, sizeof one);
    unsigned number = 0;
    ioctl(fd_, USBDEVFS_RELEASEINTERFACE, &number);
    ::close(fd_);
    fd_ = -1;
}

void USBPipe::abortRead() {
    std::lock_guard guard(lock_);
    uint64_t one = 1;
    if (open_ && abortFd_ >= 0) (void)!::write(abortFd_, &one, sizeof one);
}

bool USBPipe::write(const void *data, size_t count, unsigned timeoutMs) {
    int fd;
    { std::lock_guard guard(lock_); if (!open_) return false; fd = fd_; }
    uint8_t dummy = 0;
    usbdevfs_bulktransfer transfer{};
    transfer.ep = epOut_;
    transfer.len = unsigned(count);
    transfer.timeout = timeoutMs;
    transfer.data = count ? const_cast<void *>(data) : &dummy;
    int r = ioctl(fd, USBDEVFS_BULK, &transfer);
    if (r < 0 || size_t(r) != count) {
        unsigned ep = epOut_;
        ioctl(fd, USBDEVFS_CLEAR_HALT, &ep);   // as the original does after a failed transfer
        return false;
    }
    return true;
}

USBPipe::Read USBPipe::read(void *buffer, size_t count, unsigned timeoutMs, size_t &got) {
    got = 0;
    int fd;
    { std::lock_guard guard(lock_); if (!open_ || epIn_ == 0) return Read::aborted; fd = fd_; }

    // An asynchronous transfer, so the wait can also watch for abortRead(): the thread
    // sleeps in poll() until the camera sends something, however long that takes.
    usbdevfs_urb urb{};
    urb.type = USBDEVFS_URB_TYPE_BULK;
    urb.endpoint = epIn_;
    urb.buffer = buffer;
    urb.buffer_length = int(count);
    if (ioctl(fd, USBDEVFS_SUBMITURB, &urb) != 0) return Read::failed;

    Read outcome = Read::data;
    while (true) {
        pollfd fds[2] = {{fd, POLLOUT, 0}, {abortFd_, POLLIN, 0}};
        int r = poll(fds, 2, timeoutMs == 0 ? -1 : int(timeoutMs));
        if (r < 0 && errno == EINTR) continue;
        if (r == 0) { outcome = Read::timeout; break; }
        if (r < 0) { outcome = Read::failed; break; }
        if (fds[0].revents) break;                      // finished, or the device has gone
        if (fds[1].revents) { outcome = Read::aborted; break; }
    }
    if (outcome != Read::data) ioctl(fd, USBDEVFS_DISCARDURB, &urb);

    // Collect the transfer; after a discard this returns it cancelled. The kernel owns
    // `urb` until then, so never leave without reaping (or the device being gone).
    usbdevfs_urb *done = nullptr;
    while (ioctl(fd, USBDEVFS_REAPURB, &done) != 0) {
        if (errno == EINTR) continue;
        return outcome == Read::aborted ? Read::aborted : Read::failed;   // ENODEV: unplugged
    }
    if (outcome == Read::aborted) return Read::aborted;
    if (urb.status == 0 || (outcome == Read::timeout && urb.actual_length > 0)) {
        got = size_t(urb.actual_length);
        return Read::data;
    }
    if (outcome == Read::timeout) return Read::timeout;
    unsigned ep = epIn_;
    ioctl(fd, USBDEVFS_CLEAR_HALT, &ep);
    return Read::failed;
}
