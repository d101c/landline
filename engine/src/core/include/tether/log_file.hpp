#pragma once

#include <string>
#include <unistd.h>

// The daemon's own log file, kept in the state directory.
//
// The file is opened once at startup and written through the standard
// descriptors. A state directory that is removed while the daemon runs takes
// the file with it: the descriptors stay valid, the writes succeed, and the
// log goes to a file no path leads to any more. A directory that cannot be
// made at startup leaves no log at all.
namespace tether {

    // How often the daemon checks that its log file is still where it wrote it.
    inline constexpr int LOG_FILE_CHECK_SECONDS = 5;

    // Whether `fd` is no longer the file at `path`: the path is gone, or it
    // names another file than the one the descriptor holds.
    bool log_file_lost(const std::string& path, int fd = STDERR_FILENO);

    // Makes the directories of `path`, opens it for appending and puts it on
    // `fd` and, when not negative, on `second_fd`. Leaves both as they were
    // when the file cannot be opened.
    bool point_output_at(const std::string& path, int fd = STDERR_FILENO, int second_fd = STDOUT_FILENO);

    // point_output_at() when log_file_lost(). True when the file was opened
    // again.
    bool restore_log_file(const std::string& path, int fd = STDERR_FILENO, int second_fd = STDOUT_FILENO);

} // namespace tether
