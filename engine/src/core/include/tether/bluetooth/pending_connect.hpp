#pragma once

#include "tether/bluetooth/bearer_supervisor.hpp"

#include <mutex>
#include <string>
#include <utility>

namespace tether::bluetooth {

    // Bookkeeping for one connect issued asynchronously. The supervising thread
    // calls begin() and never waits; whichever thread receives BlueZ's answer
    // calls finish(). A failure is handed to the next begin(), once.
    class PendingConnect {
    public:
        // Requested: nothing was in flight, the caller issues the call now.
        // Busy: a call is still running. Failed: the previous call ended with `err`.
        ConnectResult begin(std::string& err) {
            std::lock_guard<std::mutex> lock(mutex_);
            if (pending_)
                return ConnectResult::Busy;
            if (!error_.empty()) {
                err = std::move(error_);
                error_.clear();
                return ConnectResult::Failed;
            }
            pending_ = true;
            return ConnectResult::Requested;
        }

        // An empty error is a call BlueZ accepted, or one whose outcome is not a refusal.
        void finish(const std::string& error = {}) {
            std::lock_guard<std::mutex> lock(mutex_);
            pending_ = false;
            error_ = error;
        }

        // A link that came up makes whatever the last attempt reported moot.
        void discard_error() {
            std::lock_guard<std::mutex> lock(mutex_);
            error_.clear();
        }

        bool pending() const {
            std::lock_guard<std::mutex> lock(mutex_);
            return pending_;
        }

    private:
        mutable std::mutex mutex_;
        bool pending_ = false;
        std::string error_;
    };

    struct ClassicReply {
        // Whether Device1.Connect should be issued next.
        bool fall_back = false;
        // What the supervisor is told; empty when there is nothing to report.
        std::string error;

        bool operator==(const ClassicReply&) const = default;
    };

    // What to do with BlueZ's answer to a BR/EDR connect. `via_bearer` is the
    // Bearer.BREDR1.Connect attempt, which falls back to Device1.Connect on a
    // refusal; `bearer_error` is what that first attempt said, the more specific
    // of the two. A D-Bus timeout is no refusal: BlueZ is still connecting.
    inline ClassicReply classify_classic_reply(bool via_bearer,
                                               bool ok,
                                               bool timed_out,
                                               const std::string& message,
                                               const std::string& bearer_error = {}) {
        if (ok || timed_out)
            return {};
        if (via_bearer) {
            if (message.find("InProgress") != std::string::npos || message.find("AlreadyConnected") != std::string::npos)
                return {false, message};
            return {true, message};
        }
        return {false, bearer_error.empty() ? message : bearer_error};
    }

} // namespace tether::bluetooth
