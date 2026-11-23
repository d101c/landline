#include "tether/bluetooth/le_conn_update.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <limits>
#include <fcntl.h>
#include <linux/capability.h>
#include <mutex>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace tether::bluetooth {

    namespace {

        // The few definitions needed from the kernel's Bluetooth headers,
        // which are not installed with the C library.
        constexpr int BT_AF = 31;       // AF_BLUETOOTH
        constexpr int BT_PROTO_HCI = 1; // BTPROTO_HCI
        constexpr int BT_SOL_HCI = 0;   // SOL_HCI
        constexpr int HCI_OPT_FILTER = 2;
        constexpr uint8_t HCI_LINK_LE = 0x80;
        constexpr uint32_t HCI_LM_ENCRYPT = 0x0004;
        constexpr int MAX_CONNECTIONS = 32;

        struct SockaddrHci {
            sa_family_t family;
            unsigned short dev;
            unsigned short channel;
        };

        struct HciFilter {
            uint32_t type_mask;
            uint32_t event_mask[2];
            uint16_t opcode;
        };

        struct __attribute__((packed)) HciConnInfo {
            uint16_t handle;
            uint8_t bdaddr[6];
            uint8_t type;
            uint8_t out;
            uint16_t state;
            uint32_t link_mode;
        };

        struct __attribute__((packed)) HciConnListReq {
            uint16_t dev_id;
            uint16_t conn_num;
            HciConnInfo conn_info[MAX_CONNECTIONS];
        };

        const unsigned long HCI_GET_CONN_LIST = _IOR('H', 212, int);

        // "AA:BB:CC:DD:EE:FF" as the kernel stores it, last byte first.
        bool parse_address(const std::string& text, uint8_t out[6]) {
            if (text.size() != 17)
                return false;
            for (int i = 0; i < 6; ++i) {
                const std::string byte = text.substr(static_cast<size_t>(i) * 3, 2);
                char* end = nullptr;
                const long value = std::strtol(byte.c_str(), &end, 16);
                if (end != byte.c_str() + 2 || (i < 5 && text[static_cast<size_t>(i) * 3 + 2] != ':'))
                    return false;
                out[5 - i] = static_cast<uint8_t>(value);
            }
            return true;
        }

        struct CapData {
            __user_cap_header_struct header{};
            __user_cap_data_struct data[2]{};
        };

        bool read_caps(CapData& caps) {
            caps.header.version = _LINUX_CAPABILITY_VERSION_3;
            caps.header.pid = 0;
            return syscall(SYS_capget, &caps.header, caps.data) == 0;
        }

        constexpr uint32_t NET_RAW_BIT = 1u << CAP_NET_RAW;

        // Raises CAP_NET_RAW in this thread for as long as it lives.
        struct RaisedNetRaw {
            bool raised = false;
            RaisedNetRaw() {
                CapData caps;
                if (!read_caps(caps) || (caps.data[0].effective & NET_RAW_BIT) ||
                    !(caps.data[0].permitted & NET_RAW_BIT))
                    return;
                caps.data[0].effective |= NET_RAW_BIT;
                raised = syscall(SYS_capset, &caps.header, caps.data) == 0;
            }
            ~RaisedNetRaw() {
                if (!raised)
                    return;
                CapData caps;
                if (!read_caps(caps))
                    return;
                caps.data[0].effective &= ~NET_RAW_BIT;
                syscall(SYS_capset, &caps.header, caps.data);
            }
        };

        LeConnUpdateResult failed(LeConnUpdateOutcome outcome, std::string detail) {
            LeConnUpdateResult result;
            result.outcome = outcome;
            result.detail = std::move(detail);
            return result;
        }

        struct Fd {
            int fd = -1;
            ~Fd() {
                if (fd >= 0)
                    close(fd);
            }
        };

    } // namespace

    NetRawRight net_raw_right() {
        CapData caps;
        if (!read_caps(caps))
            return NetRawRight::None;
        if (caps.data[0].effective & NET_RAW_BIT)
            return NetRawRight::Effective;
        return caps.data[0].permitted & NET_RAW_BIT ? NetRawRight::Permitted : NetRawRight::None;
    }

    std::string le_params_helper_path(const char* env_override) {
        return env_override && *env_override ? std::string(env_override) : std::string(LE_PARAMS_HELPER_PATH);
    }

    bool le_params_helper_usable(const std::string& path, bool strict) {
        if (path.empty() || access(path.c_str(), X_OK) != 0)
            return false;
        if (!strict)
            return true;
        struct stat info{};
        if (stat(path.c_str(), &info) != 0 || !S_ISREG(info.st_mode) || info.st_uid != 0 ||
            (info.st_mode & (S_IWGRP | S_IWOTH)) != 0)
            return false;
        // The file capability itself is not decoded: its presence is what
        // "setcap" leaves, and a helper without the right says no_right.
        return getxattr(path.c_str(), "security.capability", nullptr, 0) > 0;
    }

    namespace {

        // The controller's list of links, read once. `queried` false when it
        // could not be read at all (no controller, no address, the ioctl
        // refused), which is not the same as the phone's link being absent.
        std::optional<LeLinkInfo> lookup_le_link(int hci_index, const std::string& address, bool& queried) {
            queried = false;
            uint8_t wanted[6];
            if (hci_index < 0 || hci_index > 0xffff || !parse_address(address, wanted))
                return std::nullopt;
            Fd sock;
            sock.fd = socket(BT_AF, SOCK_RAW | SOCK_CLOEXEC, BT_PROTO_HCI);
            if (sock.fd < 0)
                return std::nullopt;
            // The links the controller holds. Reading them takes no privilege.
            auto list = std::make_unique<HciConnListReq>();
            list->dev_id = static_cast<uint16_t>(hci_index);
            list->conn_num = MAX_CONNECTIONS;
            if (ioctl(sock.fd, HCI_GET_CONN_LIST, list.get()) < 0)
                return std::nullopt;
            queried = true;
            for (int i = 0; i < std::min<int>(list->conn_num, MAX_CONNECTIONS); ++i) {
                const HciConnInfo& info = list->conn_info[i];
                // The kernel lists a bonded phone under its identity address
                // once it has resolved the private one, which it does as the
                // link completes. No other link is ever taken for the phone's.
                if (info.type == HCI_LINK_LE && std::memcmp(info.bdaddr, wanted, 6) == 0)
                    return LeLinkInfo{info.handle, (info.link_mode & HCI_LM_ENCRYPT) != 0};
            }
            return std::nullopt;
        }

    } // namespace

    std::optional<LeLinkInfo> find_le_link(int hci_index, const std::string& address) {
        bool queried = false;
        return lookup_le_link(hci_index, address, queried);
    }

    std::optional<bool> le_link_listed(int hci_index, const std::string& address) {
        bool queried = false;
        const auto link = lookup_le_link(hci_index, address, queried);
        if (!queried)
            return std::nullopt;
        return link.has_value();
    }

    namespace {

        bool stopped(const std::atomic<bool>* stop) { return stop && stop->load(); }

        // How long one poll may sleep: until the deadline, unless a stop can
        // only be seen by looking at it (no `stop_fd`), then 50 ms. An idle
        // link must not wake the computer twenty times a second (#57).
        int poll_slice(int64_t left, const std::atomic<bool>* stop, int stop_fd) {
            const int64_t most = stop && stop_fd < 0 ? 50 : std::numeric_limits<int>::max();
            return static_cast<int>(std::clamp<int64_t>(left, 0, most));
        }

        // Waits for the next packet for up to `ms`, or until `stop` (seen at
        // once through `stop_fd` when there is one). False when nothing came.
        bool wait_readable(int fd, int64_t ms, const std::atomic<bool>* stop, int stop_fd) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(std::max<int64_t>(ms, 0));
            while (!stopped(stop)) {
                const auto left =
                    std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now())
                        .count();
                if (left <= 0)
                    return false;
                pollfd watched[2] = {{fd, POLLIN, 0}, {stop_fd, POLLIN, 0}};
                const int ready = poll(watched, stop_fd >= 0 ? 2 : 1, poll_slice(left, stop, stop_fd));
                if (ready < 0 && errno == EINTR)
                    continue;
                if (ready < 0)
                    return false;
                if (watched[0].revents)
                    return true;
                if (stop_fd >= 0 && watched[1].revents)
                    return false;
            }
            return false;
        }

    } // namespace

    void hci_le_conn_update(int hci_index,
                            const std::string& address,
                            const LeConnParams& params,
                            int wait_ms,
                            int watch_seconds,
                            const LeConnUpdateSink& sink,
                            const std::atomic<bool>* stop,
                            int stop_fd) {
        if (const std::string fault = le_conn_params_fault(params); !fault.empty()) {
            sink(failed(LeConnUpdateOutcome::Failed, "parameters not sent: " + fault));
            return;
        }
        uint8_t wanted[6];
        if (hci_index < 0 || hci_index > 0xffff || !parse_address(address, wanted)) {
            sink(failed(LeConnUpdateOutcome::Failed, "no controller or no address to look for"));
            return;
        }
        const auto link = find_le_link(hci_index, address);
        if (!link) {
            sink(failed(LeConnUpdateOutcome::NoLink, "the controller holds no LE link to that address"));
            return;
        }
        const uint16_t handle = link->handle;

        Fd sock;
        sock.fd = socket(BT_AF, SOCK_RAW | SOCK_CLOEXEC, BT_PROTO_HCI);
        if (sock.fd < 0) {
            sink(failed(LeConnUpdateOutcome::Failed, std::string("HCI socket: ") + std::strerror(errno)));
            return;
        }

        RaisedNetRaw right;

        SockaddrHci addr{};
        addr.family = BT_AF;
        addr.dev = static_cast<unsigned short>(hci_index);
        addr.channel = 0; // HCI_CHANNEL_RAW
        if (bind(sock.fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            sink(failed(LeConnUpdateOutcome::Failed, std::string("HCI bind: ") + std::strerror(errno)));
            return;
        }

        // Events only, and of those the three that are about this command
        // or this link.
        HciFilter filter{};
        filter.type_mask = 1u << HCI_PKT_EVENT;
        for (uint8_t code : {HCI_EVT_DISCONN_COMPLETE, HCI_EVT_CMD_STATUS, HCI_EVT_LE_META})
            filter.event_mask[code >> 5] |= 1u << (code & 31);
        filter.opcode = HCI_OP_LE_CONN_UPDATE;
        if (setsockopt(sock.fd, BT_SOL_HCI, HCI_OPT_FILTER, &filter, sizeof(filter)) < 0) {
            sink(failed(LeConnUpdateOutcome::Failed, std::string("HCI filter: ") + std::strerror(errno)));
            return;
        }

        const auto command = le_conn_update_command(handle, params);
        ssize_t written;
        do {
            written = write(sock.fd, command.data(), command.size());
        } while (written < 0 && errno == EINTR);
        if (written < 0) {
            LeConnUpdateResult result =
                failed(errno == EPERM || errno == EACCES ? LeConnUpdateOutcome::NoRight : LeConnUpdateOutcome::Failed,
                       std::string("HCI command: ") + std::strerror(errno));
            result.handle = handle;
            sink(result);
            return;
        }

        LeConnUpdateResult result;
        result.handle = handle;
        result.outcome = LeConnUpdateOutcome::NoVerdict;
        result.detail = "the controller did not answer";
        bool verdict = false;
        bool ended = false;
        const auto verdict_by = std::chrono::steady_clock::now() + std::chrono::milliseconds(std::max(wait_ms, 0));
        while (!verdict && !stopped(stop)) {
            const auto left =
                std::chrono::duration_cast<std::chrono::milliseconds>(verdict_by - std::chrono::steady_clock::now())
                    .count();
            if (left <= 0 || !wait_readable(sock.fd, left, stop, stop_fd))
                break;
            uint8_t packet[260];
            const ssize_t got = read(sock.fd, packet, sizeof(packet));
            // A socket gone bad would be ready again at once: the wait ends.
            if (got <= 0 && !(got < 0 && (errno == EINTR || errno == EAGAIN)))
                break;
            if (got <= 0)
                continue;
            const auto event = parse_le_conn_update_event(packet, static_cast<size_t>(got), handle);
            if (!event)
                continue;
            switch (event->kind) {
            case LeConnUpdateEvent::Kind::CommandStatus:
                if (event->status != 0) {
                    result.outcome = LeConnUpdateOutcome::Refused;
                    result.status = event->status;
                    result.detail = "the controller refused the command";
                    verdict = true;
                } else {
                    result.detail = "the controller took the command; the iPhone has not answered";
                }
                break;
            case LeConnUpdateEvent::Kind::UpdateComplete:
                result.status = event->status;
                if (event->status != 0) {
                    result.outcome = LeConnUpdateOutcome::Refused;
                    result.detail = "the iPhone or the controller refused the parameters";
                } else {
                    result.outcome = LeConnUpdateOutcome::Applied;
                    result.got = event->now;
                    result.detail.clear();
                }
                verdict = true;
                break;
            case LeConnUpdateEvent::Kind::Disconnected:
                result.outcome = LeConnUpdateOutcome::NoLink;
                result.status = event->status;
                result.detail = "the link ended before the iPhone answered";
                verdict = true;
                ended = true;
                break;
            }
        }
        if (!sink(result) || ended || watch_seconds <= 0)
            return;

        // The watch: what becomes of the parameters of this link, until it
        // ends. Nothing more is sent.
        const auto watch_by = std::chrono::steady_clock::now() + std::chrono::seconds(watch_seconds);
        while (!stopped(stop)) {
            const auto left =
                std::chrono::duration_cast<std::chrono::milliseconds>(watch_by - std::chrono::steady_clock::now())
                    .count();
            // Nothing before the end of the watch, a stop, or a socket gone bad.
            if (left <= 0 || !wait_readable(sock.fd, left, stop, stop_fd))
                return;
            uint8_t packet[260];
            const ssize_t got = read(sock.fd, packet, sizeof(packet));
            // A socket gone bad would be ready again at once: the wait ends.
            if (got <= 0 && !(got < 0 && (errno == EINTR || errno == EAGAIN)))
                return;
            if (got <= 0)
                continue;
            const auto event = parse_le_conn_update_event(packet, static_cast<size_t>(got), handle);
            if (!event || event->kind == LeConnUpdateEvent::Kind::CommandStatus)
                continue;
            LeConnUpdateResult news;
            news.handle = handle;
            news.status = event->status;
            if (event->kind == LeConnUpdateEvent::Kind::Disconnected) {
                news.outcome = LeConnUpdateOutcome::Ended;
                sink(news);
                return;
            }
            if (event->status != 0)
                continue;
            news.outcome = LeConnUpdateOutcome::Changed;
            news.got = event->now;
            if (!sink(news))
                return;
        }
    }

    LeConnUpdateResult hci_le_conn_update(int hci_index, const std::string& address, const LeConnParams& params, int wait_ms) {
        LeConnUpdateResult out;
        hci_le_conn_update(hci_index, address, params, wait_ms, 0, [&out](const LeConnUpdateResult& r) {
            out = r;
            return false;
        });
        return out;
    }

    void run_le_params_helper(const std::string& path,
                              int hci_index,
                              const std::string& address,
                              const LeConnParams& params,
                              int wait_ms,
                              int watch_seconds,
                              const LeConnUpdateSink& sink,
                              const std::atomic<bool>* stop,
                              int stop_fd) {
        int out[2];
        if (pipe2(out, O_CLOEXEC) < 0) {
            sink(failed(LeConnUpdateOutcome::Failed, std::string("pipe: ") + std::strerror(errno)));
            return;
        }
        Fd reader{out[0]};

        const std::string hci = "hci" + std::to_string(hci_index);
        const std::string min = std::to_string(params.min_interval_ms);
        const std::string max = std::to_string(params.max_interval_ms);
        const std::string latency = std::to_string(params.latency);
        const std::string timeout = std::to_string(params.timeout_ms);
        const std::string wait = std::to_string(wait_ms);
        const std::string watch = std::to_string(std::max(watch_seconds, 0));
        char* const argv[] = {const_cast<char*>(path.c_str()),
                              const_cast<char*>(hci.c_str()),
                              const_cast<char*>(address.c_str()),
                              const_cast<char*>(min.c_str()),
                              const_cast<char*>(max.c_str()),
                              const_cast<char*>(latency.c_str()),
                              const_cast<char*>(timeout.c_str()),
                              const_cast<char*>(wait.c_str()),
                              const_cast<char*>(watch.c_str()),
                              nullptr};
        // Nothing of this process's environment reaches a program that
        // carries a capability.
        char* const envp[] = {nullptr};

        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_adddup2(&actions, out[1], STDOUT_FILENO);
        posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
        pid_t child = -1;
        const int spawned = posix_spawn(&child, path.c_str(), &actions, nullptr, argv, envp);
        posix_spawn_file_actions_destroy(&actions);
        close(out[1]);
        if (spawned != 0) {
            sink(failed(spawned == ENOENT || spawned == EACCES ? LeConnUpdateOutcome::NoRight : LeConnUpdateOutcome::Failed,
                        "helper " + path + ": " + std::strerror(spawned)));
            return;
        }

        std::string text;
        bool verdict = false;
        bool keep = true;
        // The verdict is due within the wait and a margin; the watch within
        // its own length and a margin.
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(std::max(wait_ms, 0) + 3000);
        bool timed_out = false;
        while (keep) {
            if (stopped(stop))
                break;
            const auto left =
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
            if (left <= 0) {
                timed_out = true;
                break;
            }
            pollfd watched[2] = {{reader.fd, POLLIN | POLLHUP, 0}, {stop_fd, POLLIN, 0}};
            const int ready = poll(watched, stop_fd >= 0 ? 2 : 1, poll_slice(left, stop, stop_fd));
            if (ready < 0 && errno == EINTR)
                continue;
            if (ready < 0)
                break;
            if (!watched[0].revents) {
                if (stop_fd >= 0 && watched[1].revents)
                    break;
                continue;
            }
            char chunk[512];
            const ssize_t got = read(reader.fd, chunk, sizeof(chunk));
            if (got < 0 && errno == EINTR)
                continue;
            if (got <= 0)
                break;
            text.append(chunk, static_cast<size_t>(got));
            size_t end;
            while (keep && (end = text.find('\n')) != std::string::npos) {
                const LeConnUpdateResult result = parse_le_conn_update_result(std::string_view(text).substr(0, end));
                text.erase(0, end + 1);
                if (!verdict) {
                    verdict = true;
                    deadline = std::chrono::steady_clock::now() + std::chrono::seconds(std::max(watch_seconds, 0) + 30);
                }
                keep = sink(result);
            }
            // A helper that writes without end is not listened to.
            if (text.size() > 4096)
                break;
        }
        // Still running: a watch that was stopped, or a helper that said
        // nothing in time. Its link is left as it is.
        if (kill(child, 0) == 0)
            kill(child, SIGTERM);
        int status = 0;
        for (int i = 0; i < 20; ++i) {
            const pid_t done = waitpid(child, &status, WNOHANG);
            if (done == child || (done < 0 && errno != EINTR))
                break;
            if (i == 19) {
                kill(child, SIGKILL);
                while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
                }
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        if (!verdict && !stopped(stop))
            sink(failed(LeConnUpdateOutcome::Failed,
                        timed_out ? "the helper did not finish in time" : "the helper printed nothing that could be read"));
    }

    struct LeConnUpdater::State {
        NetRawRight right = NetRawRight::None;
        std::string helper;
        bool helper_usable = false;

        std::thread worker;
        std::atomic<bool> running{false};
        std::atomic<bool> requesting{false};
        std::atomic<bool> stop{false};
        // Readable once `stop` is set, so the worker's waits can sleep
        // without ticking (#57).
        int wake_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
        std::mutex mutex;

        ~State() {
            if (wake_fd >= 0)
                close(wake_fd);
        }
        std::deque<LeConnUpdateResult> news;

        void push(const LeConnUpdateResult& result) {
            std::lock_guard<std::mutex> lock(mutex);
            if (news.size() < 64)
                news.push_back(result);
        }
    };

    LeConnUpdater::LeConnUpdater() : state_(std::make_shared<State>()) {
        state_->right = net_raw_right();
        const char* override_path = std::getenv("LANDLINE_LE_PARAMS_HELPER");
        state_->helper = le_params_helper_path(override_path);
        state_->helper_usable = le_params_helper_usable(state_->helper, !(override_path && *override_path));
    }

    LeConnUpdater::~LeConnUpdater() { stop(); }

    bool LeConnUpdater::able() const { return state_->right != NetRawRight::None || state_->helper_usable; }

    const char* LeConnUpdater::way() const { return state_->right != NetRawRight::None ? "capability" : "helper"; }

    const std::string& LeConnUpdater::helper() const { return state_->helper; }

    bool LeConnUpdater::requesting() const { return state_->requesting.load(); }

    bool LeConnUpdater::active() const { return state_->running.load(); }

    void LeConnUpdater::stop() {
        state_->stop = true;
        if (state_->wake_fd >= 0) {
            const uint64_t one = 1;
            [[maybe_unused]] const ssize_t ignored = write(state_->wake_fd, &one, sizeof(one));
        }
        if (state_->worker.joinable())
            state_->worker.join();
        if (state_->wake_fd >= 0) {
            uint64_t drained = 0;
            [[maybe_unused]] const ssize_t ignored = read(state_->wake_fd, &drained, sizeof(drained));
        }
        state_->stop = false;
        state_->running = false;
        state_->requesting = false;
    }

    void LeConnUpdater::start(int hci_index,
                              const std::string& address,
                              const LeConnParams& params,
                              bool wait_encryption,
                              int watch_seconds) {
        stop();
        if (!able()) {
            LeConnUpdateResult result;
            result.outcome = LeConnUpdateOutcome::NoRight;
            result.detail = "no CAP_NET_RAW and no usable helper at " + state_->helper;
            state_->push(result);
            return;
        }
        state_->running = true;
        state_->requesting = true;
        std::shared_ptr<State> state = state_;
        state_->worker = std::thread([state, hci_index, address, params, wait_encryption, watch_seconds] {
            // The link as the kernel lists it: there, and encrypted when
            // that is waited for. Looked at every 50 ms: a young link at
            // 720 ms of supervision has no time to spare.
            const auto by = std::chrono::steady_clock::now() + std::chrono::milliseconds(LE_CONN_PARAMS_ENCRYPTION_WAIT_MS);
            std::optional<LeLinkInfo> link;
            while (!state->stop.load()) {
                link = find_le_link(hci_index, address);
                if (link && (link->encrypted || !wait_encryption))
                    break;
                if (std::chrono::steady_clock::now() >= by)
                    break;
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            if (state->stop.load()) {
                state->requesting = false;
                state->running = false;
                return;
            }
            if (!link || (wait_encryption && !link->encrypted)) {
                LeConnUpdateResult result;
                result.outcome = link ? LeConnUpdateOutcome::NotReady : LeConnUpdateOutcome::NoLink;
                if (link)
                    result.handle = link->handle;
                result.detail = link ? "the LE link was not encrypted in time; nothing was sent"
                                     : "the controller holds no LE link to the iPhone";
                state->push(result);
                state->requesting = false;
                state->running = false;
                return;
            }
            const LeConnUpdateSink sink = [state](const LeConnUpdateResult& result) {
                state->push(result);
                state->requesting = false;
                return !state->stop.load();
            };
            if (state->right != NetRawRight::None)
                hci_le_conn_update(
                    hci_index, address, params, LE_CONN_PARAMS_VERDICT_MS, watch_seconds, sink, &state->stop, state->wake_fd);
            else
                run_le_params_helper(
                    state->helper, hci_index, address, params, LE_CONN_PARAMS_VERDICT_MS, watch_seconds, sink, &state->stop, state->wake_fd);
            state->requesting = false;
            state->running = false;
        });
    }

    std::optional<LeConnUpdateResult> LeConnUpdater::take() {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (state_->news.empty())
            return std::nullopt;
        LeConnUpdateResult out = state_->news.front();
        state_->news.pop_front();
        return out;
    }

} // namespace tether::bluetooth
