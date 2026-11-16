#pragma once

#include <filesystem>
#include <functional>
#include <gio/gio.h>
#include <string>

namespace tether::bluetooth {

    // obexd removes a transfer object as soon as it finishes, so an object that
    // has disappeared is a normal terminal state rather than a failure.
    enum class TransferState { Active, Complete, Error, Gone };

    // Polls a transfer to completion. Returns Active only when the timeout ran
    // out with the transfer still in progress.
    TransferState wait_for_transfer(GDBusConnection* bus, const std::string& path, int timeout_seconds);

    // For as long as it lives, the calling thread runs `idle` every time it
    // finds a transfer still in progress in wait_for_transfer(), about twenty
    // times a second. A long read holds its thread for seconds; this is how
    // that thread goes on doing what must not wait. Other threads are not
    // affected, and `idle` is not entered again from inside itself.
    class TransferIdleScope {
    public:
        explicit TransferIdleScope(std::function<void()> idle);
        ~TransferIdleScope();

        TransferIdleScope(const TransferIdleScope&) = delete;
        TransferIdleScope& operator=(const TransferIdleScope&) = delete;

    private:
        std::function<void()> previous_;
    };

    // Runs the calling thread's idle function, if any. For wait_for_transfer().
    void run_transfer_idle();

    // Where to stage a file obexd reads or writes.
    std::filesystem::path obex_staging_path(const std::string& name);

} // namespace tether::bluetooth
