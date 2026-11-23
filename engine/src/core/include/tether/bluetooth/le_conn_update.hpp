#pragma once

#include "tether/bluetooth/le_conn_params.hpp"

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <string>

// Asking the controller for other connection parameters on the LE link
// (le_conn_params.hpp): the part that touches the system.
//
// The command goes out on a raw HCI socket, and the kernel takes an LE
// command there only from a thread holding CAP_NET_RAW
// (net/bluetooth/hci_sock.c, hci_sock_sendmsg). Two ways to hold it:
//   - the helper landline-le-params, installed root-owned by the system
//     assistant with cap_net_raw on its file, run for each request (the way
//     meant for use);
//   - this program's own file carries the capability (permitted is enough:
//     it is raised in the one short-lived thread that writes the command).
// Without either the request is not made, which is said once and is not an
// error.
//
// Finding the link takes no privilege: the kernel lists the links of a
// controller (HCIGETCONNLIST, what "hcitool con" reads) to anyone, with
// their handle, address, type and whether they are encrypted. The link is
// looked up by the phone's address, never by a handle handed in.
namespace tether::bluetooth {

    enum class NetRawRight {
        // In the effective set of the calling thread.
        Effective,
        // Permitted only: it can be raised.
        Permitted,
        None,
    };
    NetRawRight net_raw_right();

    // Where the helper is looked for: LANDLINE_LE_PARAMS_HELPER when set,
    // the path the system assistant installs it under otherwise.
    inline constexpr const char* LE_PARAMS_HELPER_PATH = "/usr/local/libexec/landline/landline-le-params";
    std::string le_params_helper_path(const char* env_override);

    // Whether the helper at `path` can do its work: executable, and, unless
    // `strict` is false (a path set in the environment, for trials), owned by
    // root, writable by nobody else and carrying a file capability.
    bool le_params_helper_usable(const std::string& path, bool strict);

    // The LE link to `address` ("AA:BB:CC:DD:EE:FF") on controller
    // `hci_index`, as the kernel lists it. Nothing when there is none, or
    // when the list cannot be read.
    struct LeLinkInfo {
        uint16_t handle = 0;
        bool encrypted = false;
    };
    std::optional<LeLinkInfo> find_le_link(int hci_index, const std::string& address);

    // Whether the controller lists an LE link to `address` (#63): nothing
    // when the list could not be read, which must never pass for "absent".
    std::optional<bool> le_link_listed(int hci_index, const std::string& address);

    // Called with each line's worth of news: the verdict first, then, while
    // the link is watched, each change of its parameters and its end.
    // Returning false stops the watch.
    using LeConnUpdateSink = std::function<bool(const LeConnUpdateResult&)>;

    // Finds the LE link to `address` on controller `hci_index`, sends LE
    // Connection Update on it and waits up to `wait_ms` for what the phone
    // decided. Then, for up to `watch_seconds`, reports each LE Connection
    // Update Complete on that link and stops at its end. Blocks; `stop` ends
    // the watch early. With `stop_fd`, readable once `stop` is set, the wait
    // sleeps until a packet, the stop or the deadline; without it, `stop` is
    // looked at every 50 ms. With no `stop` at all, nothing ticks. Never ends a link
    // and sends nothing else. Raises CAP_NET_RAW in the calling thread when
    // it is only permitted.
    void hci_le_conn_update(int hci_index,
                            const std::string& address,
                            const LeConnParams& params,
                            int wait_ms,
                            int watch_seconds,
                            const LeConnUpdateSink& sink,
                            const std::atomic<bool>* stop = nullptr,
                            int stop_fd = -1);

    // The request alone, no watch: the verdict.
    LeConnUpdateResult hci_le_conn_update(int hci_index, const std::string& address, const LeConnParams& params, int wait_ms);

    // The same through the helper at `path`, which prints one line per
    // result. Its environment is empty and its input /dev/null. `stop` and
    // `stop_fd` as above.
    void run_le_params_helper(const std::string& path,
                              int hci_index,
                              const std::string& address,
                              const LeConnParams& params,
                              int wait_ms,
                              int watch_seconds,
                              const LeConnUpdateSink& sink,
                              const std::atomic<bool>* stop = nullptr,
                              int stop_fd = -1);

    // One request at a time, and the watch that follows it, off the
    // caller's thread: the controller's verdict can take seconds.
    class LeConnUpdater {
    public:
        LeConnUpdater();
        ~LeConnUpdater();

        LeConnUpdater(const LeConnUpdater&) = delete;
        LeConnUpdater& operator=(const LeConnUpdater&) = delete;

        // Whether a request could be made at all: the capability, or a
        // helper that can do it. Read once.
        bool able() const;
        // "capability" or "helper", for the journal.
        const char* way() const;
        // Where the helper is looked for.
        const std::string& helper() const;

        // Waits for the LE link to `address` to be listed (and encrypted,
        // with `wait_encryption`), then makes the request and watches the
        // link for `watch_seconds`. Ends whatever ran before.
        void start(int hci_index,
                   const std::string& address,
                   const LeConnParams& params,
                   bool wait_encryption,
                   int watch_seconds = LE_CONN_PARAMS_WATCH_SECONDS);
        // The request is on its way: no verdict yet.
        bool requesting() const;
        // A request or a watch is running.
        bool active() const;
        // Ends what runs, and waits for it.
        void stop();
        // The next piece of news, in order, once.
        std::optional<LeConnUpdateResult> take();

    private:
        struct State;
        std::shared_ptr<State> state_;
    };

} // namespace tether::bluetooth
