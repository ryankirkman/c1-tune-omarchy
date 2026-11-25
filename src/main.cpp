// c1-tune: real hardware controls for the Opal C1 webcam on Omarchy.
//
// Run with no arguments it is the app: it boots the camera into DepthAI firmware, keeps it
// fed, and applies the saved settings. Every other invocation (--set, --status, --panel, …)
// is a short-lived client talking to that app over a Unix socket.

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <map>
#include <mutex>
#include <poll.h>
#include <string>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include "log.h"
#include "session.h"
#include "settings.h"

#ifndef C1TUNE_VERSION
#define C1TUNE_VERSION "dev"
#define C1TUNE_COMMIT "dev"
#endif

namespace {

std::string socketPath() {
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    return (runtime && *runtime ? std::string(runtime) : "/tmp") + "/c1-tune.sock";
}

int connectToApp() {
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    strncpy(address.sun_path, socketPath().c_str(), sizeof address.sun_path - 1);
    if (connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof address) != 0) { close(fd); return -1; }
    return fd;
}

std::string jsonString(const std::string &s) {
    std::string out = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { out += '\\'; out += char(c); }
        else if (c < 0x20) out += ' ';
        else out += char(c);
    }
    return out + "\"";
}

// MARK: - The app

/// Owns the settings and the camera session, and serves clients (the panel, --set).
/// One thread, asleep in epoll until a client, the session or a timer has something.
class App {
public:
    explicit App(bool verbose)
        : settings_(CameraSettings::load()),
          session_(settings_, [this](Session::Status s) { statusFromSession(s); }, verbose) {}

    int run() {
        listenFd_ = listenSocket();
        if (listenFd_ < 0) return 1;

        epoll_ = epoll_create1(EPOLL_CLOEXEC);
        statusFd_ = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
        restartTimer_ = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
        sigset_t signals;
        sigemptyset(&signals);
        sigaddset(&signals, SIGTERM);
        sigaddset(&signals, SIGINT);
        sigaddset(&signals, SIGHUP);
        sigprocmask(SIG_BLOCK, &signals, nullptr);
        signal(SIGPIPE, SIG_IGN);
        int signalFd = signalfd(-1, &signals, SFD_CLOEXEC | SFD_NONBLOCK);
        for (int fd : {listenFd_, statusFd_, restartTimer_, signalFd}) watch(fd);

        session_.start();

        while (!quit_) {
            epoll_event events[16];
            int n = epoll_wait(epoll_, events, 16, -1);
            if (n < 0 && errno != EINTR) break;
            for (int i = 0; i < n; i++) {
                int fd = events[i].data.fd;
                if (fd == listenFd_) acceptClient();
                else if (fd == statusFd_) { drain(fd); broadcast(); }
                else if (fd == restartTimer_) { drain(fd); restartPending_ = false; session_.restart(settings_); broadcast(); }
                else if (fd == signalFd) quit_ = true;
                else readClient(fd);
            }
        }
        unlink(socketPath().c_str());
        session_.releaseCamera();
        return 0;
    }

private:
    struct Client { std::string input; bool watching = false; };

    int listenSocket() {
        // Only one copy may drive the camera.
        if (int existing = connectToApp(); existing >= 0) {
            close(existing);
            fprintf(stderr, "C1 Tune is already running.\n");
            return -1;
        }
        unlink(socketPath().c_str());
        int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        strncpy(address.sun_path, socketPath().c_str(), sizeof address.sun_path - 1);
        mode_t old = umask(0077);
        bool ok = fd >= 0 && bind(fd, reinterpret_cast<sockaddr *>(&address), sizeof address) == 0 && listen(fd, 8) == 0;
        umask(old);
        if (!ok) { perror("c1-tune: socket"); if (fd >= 0) close(fd); return -1; }
        return fd;
    }

    void watch(int fd) {
        epoll_event event{};
        event.events = EPOLLIN;
        event.data.fd = fd;
        epoll_ctl(epoll_, EPOLL_CTL_ADD, fd, &event);
    }

    static void drain(int fd) { uint64_t value; (void)!read(fd, &value, sizeof value); }

    // Called on the session's thread.
    void statusFromSession(Session::Status status) {
        { std::lock_guard guard(statusLock_); status_ = std::move(status); }
        uint64_t one = 1;
        (void)!write(statusFd_, &one, sizeof one);
    }

    std::string stateLine() {
        Session::Status status;
        { std::lock_guard guard(statusLock_); status = status_; }
        static const char *kinds[] = {"notConnected", "starting", "ready", "busy", "failed"};
        std::string zooms;
        for (auto &stop : CameraSettings::zoomStops) zooms += (zooms.empty() ? "" : ",") + std::to_string(stop.percent);
        return std::string("{\"status\":\"") + kinds[status.kind] + "\",\"message\":" + jsonString(status.message)
            + ",\"restartPending\":" + (restartPending_ ? "true" : "false")
            + ",\"version\":\"" C1TUNE_VERSION " (" C1TUNE_COMMIT ")\",\"zoomStops\":[" + zooms + "],\"settings\":" + settings_.json() + "}\n";
    }

    void broadcast() {
        std::string line = stateLine();
        std::vector<int> gone;
        for (auto &[fd, client] : clients_)
            if (client.watching && send(fd, line.data(), line.size(), MSG_NOSIGNAL | MSG_DONTWAIT) < 0 && errno != EAGAIN) gone.push_back(fd);
        for (int fd : gone) dropClient(fd);
    }

    void acceptClient() {
        int fd = accept4(listenFd_, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
        if (fd < 0) return;
        clients_[fd] = {};
        watch(fd);
    }

    void dropClient(int fd) {
        epoll_ctl(epoll_, EPOLL_CTL_DEL, fd, nullptr);
        close(fd);
        clients_.erase(fd);
    }

    void readClient(int fd) {
        auto it = clients_.find(fd);
        if (it == clients_.end()) return;
        char buf[4096];
        ssize_t n = recv(fd, buf, sizeof buf, 0);
        if (n <= 0) { if (n == 0 || (errno != EAGAIN && errno != EINTR)) dropClient(fd); return; }
        it->second.input.append(buf, size_t(n));
        if (it->second.input.size() > 65536) { dropClient(fd); return; }
        size_t newline;
        while (clients_.count(fd) && (newline = clients_[fd].input.find('\n')) != std::string::npos) {
            std::string line = clients_[fd].input.substr(0, newline);
            clients_[fd].input.erase(0, newline + 1);
            command(fd, line);
        }
    }

    void reply(int fd, const std::string &text) { (void)!send(fd, text.data(), text.size(), MSG_NOSIGNAL | MSG_DONTWAIT); }

    /// One request per line: a verb, then tab-separated arguments.
    void command(int fd, const std::string &line) {
        std::vector<std::string> parts;
        for (size_t start = 0; start <= line.size();) {
            size_t tab = line.find('\t', start);
            if (tab == std::string::npos) tab = line.size();
            parts.push_back(line.substr(start, tab - start));
            start = tab + 1;
        }
        const std::string &verb = parts[0];
        if (verb == "watch") {
            clients_[fd].watching = true;
            reply(fd, stateLine());
        } else if (verb == "get") {
            reply(fd, stateLine());
        } else if (verb == "set") {
            std::vector<std::pair<std::string, std::string>> pairs;
            for (size_t i = 1; i < parts.size(); i++) {
                size_t eq = parts[i].find('=');
                if (eq != std::string::npos) pairs.emplace_back(parts[i].substr(0, eq), parts[i].substr(eq + 1));
            }
            change(settings_.applying(pairs));
            reply(fd, "ok\n");
        } else if (verb == "reset") {
            reset();
            reply(fd, "ok\n");
        } else if (verb == "restart") {
            itimerspec never{};
            timerfd_settime(restartTimer_, 0, &never, nullptr);
            restartPending_ = false;
            session_.restart(settings_);
            reply(fd, "ok\n");
            broadcast();
        } else if (verb == "retry") {
            session_.poke();
            reply(fd, "ok\n");
        } else if (verb == "quit") {
            reply(fd, "ok\n");
            quit_ = true;
        } else {
            reply(fd, "unknown\n");
        }
    }

    /// New settings from the panel or --set: sent straight to the camera, or, when they need
    /// the pipeline rebuilt, applied a moment after the last change so adjusting them costs
    /// one restart, not several.
    void change(const CameraSettings &updated) {
        if (updated == settings_) return;
        bool restart = settings_.needsRestart(updated);
        std::vector<CameraSettings::Group> groups;
        for (int g = 0; g < CameraSettings::groupCount; g++) {
            auto group = CameraSettings::Group(g);
            if (!(settings_.control(group) == updated.control(group))) groups.push_back(group);
        }
        settings_ = updated;
        if (!settings_.save()) logLine("couldn't save %s", CameraSettings::path().c_str());
        if (restart) {
            restartPending_ = true;
            itimerspec when{};
            when.it_value.tv_sec = 1;
            timerfd_settime(restartTimer_, 0, &when, nullptr);
        }
        // Live commands go out even with a restart on its way: the picture follows at once,
        // and the restart builds its pipeline from the same settings.
        if (!groups.empty()) session_.send(groups, settings_);
        broadcast();
    }

    /// Back to defaults. Restarts the camera's pipeline so values without a "default"
    /// command (sharpness, denoise) return to the tuning's own.
    void reset() {
        itimerspec never{};
        timerfd_settime(restartTimer_, 0, &never, nullptr);   // the restart below covers it
        restartPending_ = false;
        CameraSettings defaults;
        defaults.tuningFile = settings_.tuningFile;
        settings_ = defaults;
        settings_.save();
        session_.restart(settings_);
        broadcast();
    }

    CameraSettings settings_;
    Session session_;
    std::mutex statusLock_;
    Session::Status status_{Session::Status::starting, "Looking for the camera…"};   // guarded by statusLock_
    std::map<int, Client> clients_;
    int epoll_ = -1, listenFd_ = -1, statusFd_ = -1, restartTimer_ = -1;
    bool restartPending_ = false;
    bool quit_ = false;
};

// MARK: - Clients

/// Sends one request to the running app and prints its one-line answer.
int request(const std::string &line, bool print) {
    int fd = connectToApp();
    if (fd < 0) {
        fprintf(stderr, "C1 Tune isn't running; start it first (c1-tune &, or open it from the launcher).\n");
        return 1;
    }
    std::string out = line + "\n", answer;
    (void)!write(fd, out.data(), out.size());
    pollfd p{fd, POLLIN, 0};
    char buf[4096];
    while (answer.find('\n') == std::string::npos && poll(&p, 1, 3000) > 0) {
        ssize_t n = read(fd, buf, sizeof buf);
        if (n <= 0) break;
        answer.append(buf, size_t(n));
    }
    close(fd);
    if (answer.empty()) { fprintf(stderr, "C1 Tune didn't answer.\n"); return 1; }
    if (print) fputs(answer.c_str(), stdout);
    return 0;
}

std::string siblingPath(const char *name) {
    char self[4096];
    ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
    std::string path = n > 0 ? std::string(self, size_t(n)) : "c1-tune";
    return path.substr(0, path.rfind('/') + 1) + name;
}

/// Starts the app detached from this process if it isn't running yet.
void ensureAppRunning() {
    if (int fd = connectToApp(); fd >= 0) { close(fd); return; }
    pid_t child = fork();
    if (child == 0) {
        setsid();
        if (fork() != 0) _exit(0);
        int null = open("/dev/null", O_RDWR);
        if (null >= 0) { dup2(null, 0); dup2(null, 1); dup2(null, 2); }
        std::string self = siblingPath("c1-tune");
        execl(self.c_str(), "c1-tune", nullptr);
        _exit(127);
    }
    if (child > 0) waitpid(child, nullptr, 0);   // the go-between exits at once; don't leave it a zombie
    for (int i = 0; i < 40; i++) {   // up to a second for its socket to appear
        if (int fd = connectToApp(); fd >= 0) { close(fd); return; }
        usleep(25000);
    }
}

int usage(FILE *out) {
    fputs("C1 Tune " C1TUNE_VERSION " (" C1TUNE_COMMIT "): hardware controls for the Opal C1 webcam.\n\n"
          "  c1-tune                     run the app (keeps the camera booted and tuned)\n"
          "  c1-tune --panel             open or close the control panel, starting the app if needed\n"
          "  c1-tune --set key=value …   change settings on the running app\n"
          "  c1-tune --status            print the app's state and settings as JSON\n"
          "  c1-tune --reset             back to the default settings\n"
          "  c1-tune --retry             look for the camera again after a failure\n"
          "  c1-tune --restart           restart the camera's pipeline\n"
          "  c1-tune --fetch-tuning      download Opal's image tuning from Opal (once) and use it\n"
          "  c1-tune --quit              stop the app (the camera returns to its factory webcam mode)\n"
          "  c1-tune --verbose           run the app, logging the camera's own messages too\n\n"
          "Keys: autoExposure exposureUs iso exposureCompensation autoFocus lensPosition autoWhiteBalance\n"
          "whiteBalanceK brightness contrast saturation sharpness noiseReduction colorNoiseReduction\n"
          "antiFlicker zoom frameX frameY flipped microphone tuningFile\n"
          "`default` clears sharpness, a denoise value or tuningFile back to the firmware's own.\n", out);
    return out == stdout ? 0 : 2;
}

}

int main(int argc, char **argv) {
    std::string mode = argc > 1 ? argv[1] : "";
    if (mode.empty() || mode == "--verbose" || mode == "-v") return App(!mode.empty()).run();
    if (mode == "--help" || mode == "-h") return usage(stdout);
    if (mode == "--version") { puts(C1TUNE_VERSION " (" C1TUNE_COMMIT ")"); return 0; }
    if (mode == "--set") {
        // Tabs separate pairs on the wire, so a value may hold spaces (a tuning file's path).
        std::string line = "set";
        for (int i = 2; i < argc; i++) {
            std::string pair = argv[i];
            if (pair.find('=') == std::string::npos || pair.find_first_of("\t\n") != std::string::npos) continue;
            line += "\t" + pair;
        }
        return request(line, false);
    }
    if (mode == "--status") return request("get", true);
    if (mode == "--reset") return request("reset", false);
    if (mode == "--retry") return request("retry", false);
    if (mode == "--restart") return request("restart", false);
    if (mode == "--fetch-tuning") {
        // Installed under share/ beside bin/ (/usr or ~/.local); in a checkout, under scripts/.
        for (std::string script : {siblingPath("../share/c1-tune/fetch-opal-tuning"), siblingPath("../scripts/fetch-opal-tuning")}) {
            if (access(script.c_str(), X_OK) != 0) continue;
            pid_t child = fork();
            if (child == 0) { execl(script.c_str(), script.c_str(), nullptr); _exit(127); }
            int status = 0;
            waitpid(child, &status, 0);
            if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) return 1;
            if (int fd = connectToApp(); fd >= 0) { close(fd); return request("restart", false); }
            return 0;
        }
        fprintf(stderr, "fetch-opal-tuning isn't installed.\n");
        return 1;
    }
    if (mode == "--quit") return request("quit", false);
    if (mode == "--panel") {
        ensureAppRunning();
        std::string panel = siblingPath("c1-tune-panel");
        execl(panel.c_str(), "c1-tune-panel", nullptr);
        fprintf(stderr, "Couldn't start %s: %s\n", panel.c_str(), strerror(errno));
        return 1;
    }
    return usage(stderr);
}
