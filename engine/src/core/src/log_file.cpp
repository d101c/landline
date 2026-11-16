#include "tether/log_file.hpp"
#include "tether/log.hpp"

#include <fcntl.h>
#include <filesystem>
#include <sys/stat.h>

namespace tether {

    bool log_file_lost(const std::string& path, int fd) {
        struct stat on_disk {};
        struct stat held {};
        if (stat(path.c_str(), &on_disk) != 0)
            return true;
        if (fstat(fd, &held) != 0)
            return true;
        return on_disk.st_dev != held.st_dev || on_disk.st_ino != held.st_ino;
    }

    bool point_output_at(const std::string& path, int fd, int second_fd) {
        std::error_code ec;
        const std::filesystem::path parent = std::filesystem::path(path).parent_path();
        if (!parent.empty())
            std::filesystem::create_directories(parent, ec);

        const int file = open(path.c_str(), O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0644);
        if (file < 0)
            return false;

        // dup2 swaps the descriptor in one step: a line being written by
        // another thread lands in the old file or the new one, never nowhere.
        // The lock keeps a line from being split across the two.
        bool ok = true;
        {
            std::lock_guard<std::mutex> lock(debug::log_mutex);
            ok = dup2(file, fd) >= 0;
            if (ok && second_fd >= 0)
                dup2(file, second_fd);
        }
        if (file != fd && file != second_fd)
            close(file);
        return ok;
    }

    bool restore_log_file(const std::string& path, int fd, int second_fd) {
        if (!log_file_lost(path, fd))
            return false;
        return point_output_at(path, fd, second_fd);
    }

} // namespace tether
