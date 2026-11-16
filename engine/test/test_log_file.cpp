#include <gtest/gtest.h>
#include <tether/log_file.hpp>

#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {

    // A descriptor of its own for each test: the process's standard output
    // and error are never touched.
    struct LogFixture : ::testing::Test {
        fs::path root;
        fs::path path;
        int fd = -1;

        void SetUp() override {
            char pattern[] = "/tmp/landline-logfile-XXXXXX";
            root = mkdtemp(pattern);
            path = root / "state" / "landline" / "landlined.log";
            fd = open("/dev/null", O_WRONLY | O_CLOEXEC);
            ASSERT_GE(fd, 0);
        }

        void TearDown() override {
            if (fd >= 0)
                close(fd);
            std::error_code ec;
            fs::remove_all(root, ec);
        }

        void say(const std::string& line) const { ASSERT_EQ(write(fd, line.data(), line.size()), ssize_t(line.size())); }

        std::string contents() const {
            std::ifstream in(path);
            std::stringstream text;
            text << in.rdbuf();
            return text.str();
        }
    };

} // namespace

TEST_F(LogFixture, MakesTheDirectoryThatIsNotThereAtStartup) {
    ASSERT_FALSE(fs::exists(path.parent_path()));
    EXPECT_TRUE(tether::point_output_at(path.string(), fd, -1));
    say("first line\n");
    EXPECT_EQ(contents(), "first line\n");
    EXPECT_FALSE(tether::log_file_lost(path.string(), fd));
}

TEST_F(LogFixture, AppendsToWhatIsAlreadyThere) {
    fs::create_directories(path.parent_path());
    std::ofstream(path) << "earlier\n";
    ASSERT_TRUE(tether::point_output_at(path.string(), fd, -1));
    say("later\n");
    EXPECT_EQ(contents(), "earlier\nlater\n");
}

// What happened on 16 November: the state directory was removed while the
// engine ran, and the incident that followed left no log.
TEST_F(LogFixture, ADirectoryRemovedUnderTheDaemonIsMadeAgain) {
    ASSERT_TRUE(tether::point_output_at(path.string(), fd, -1));
    say("before\n");

    fs::remove_all(path.parent_path());
    EXPECT_TRUE(tether::log_file_lost(path.string(), fd));
    say("into the void\n"); // succeeds, and is kept nowhere

    EXPECT_TRUE(tether::restore_log_file(path.string(), fd, -1));
    say("after\n");
    EXPECT_EQ(contents(), "after\n");
    EXPECT_FALSE(tether::log_file_lost(path.string(), fd));
}

TEST_F(LogFixture, AFileReplacedByAnotherIsFollowed) {
    ASSERT_TRUE(tether::point_output_at(path.string(), fd, -1));
    say("old file\n");
    fs::rename(path, root / "rotated.log");
    std::ofstream(path) << "";
    EXPECT_TRUE(tether::log_file_lost(path.string(), fd));
    EXPECT_TRUE(tether::restore_log_file(path.string(), fd, -1));
    say("new file\n");
    EXPECT_EQ(contents(), "new file\n");
}

TEST_F(LogFixture, NothingIsReopenedWhileTheFileIsInPlace) {
    ASSERT_TRUE(tether::point_output_at(path.string(), fd, -1));
    EXPECT_FALSE(tether::restore_log_file(path.string(), fd, -1));
    say("one\n");
    EXPECT_FALSE(tether::restore_log_file(path.string(), fd, -1));
    say("two\n");
    EXPECT_EQ(contents(), "one\ntwo\n");
}

TEST_F(LogFixture, BothDescriptorsFollow) {
    const int second = open("/dev/null", O_WRONLY | O_CLOEXEC);
    ASSERT_GE(second, 0);
    ASSERT_TRUE(tether::point_output_at(path.string(), fd, second));
    say("from the first\n");
    const std::string line = "from the second\n";
    ASSERT_EQ(write(second, line.data(), line.size()), ssize_t(line.size()));
    EXPECT_EQ(contents(), "from the first\nfrom the second\n");
    close(second);
}

TEST_F(LogFixture, APathThatCannotBeMadeLeavesTheDescriptorAlone) {
    std::ofstream(root / "blocker") << "a file where a directory is wanted";
    const std::string impossible = (root / "blocker" / "landlined.log").string();
    EXPECT_FALSE(tether::point_output_at(impossible, fd, -1));
    EXPECT_TRUE(tether::log_file_lost(impossible, fd));
    say("still writable\n");
}
