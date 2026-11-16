#include "scoped_env.hpp"

#include <fstream>
#include <gtest/gtest.h>
#include <tether/service.hpp>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {

    class ServiceTest : public ::testing::Test {
    protected:
        void SetUp() override { fs::create_directories(config_); }

        void TearDown() override {
            std::error_code ec;
            fs::remove_all(root_, ec);
        }

        const fs::path root_ =
            fs::temp_directory_path() / ("landline-service-" + std::to_string(::getpid()) + "-" +
                                         ::testing::UnitTest::GetInstance()->current_test_info()->name());
        const fs::path config_ = root_ / "config";
        const fs::path unit_ = config_ / "systemd/user/landlined.service";
    };

    std::string read(const fs::path& path) {
        std::ifstream in(path);
        return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    }

} // namespace

TEST_F(ServiceTest, RenderTetherdUnitFillsInExecStart) {
    const std::string unit = tether::render_tetherd_unit("/usr/bin/landlined");

    EXPECT_NE(unit.find("ExecStart=/usr/bin/landlined"), std::string::npos) << unit;
    EXPECT_EQ(unit.find("__TETHERD_EXEC__"), std::string::npos) << unit;
    EXPECT_NE(unit.find("WantedBy=default.target"), std::string::npos) << unit;
}

TEST_F(ServiceTest, InstallWritesTheUnitAndUninstallTakesItBack) {
    const auto installed = tether::install_tetherd_service(config_);

    ASSERT_TRUE(installed.errors.empty()) << installed.errors.front();
    EXPECT_TRUE(installed.changed);
    EXPECT_EQ(installed.path, unit_.string());
    ASSERT_TRUE(fs::exists(unit_));
    EXPECT_NE(read(unit_).find("ExecStart="), std::string::npos);

    const auto removed = tether::uninstall_tetherd_service(config_);
    EXPECT_TRUE(removed.errors.empty());
    EXPECT_TRUE(removed.changed);
    EXPECT_FALSE(fs::exists(unit_));
}

TEST_F(ServiceTest, UninstallingWhatWasNeverInstalledIsNotAnError) {
    const auto removed = tether::uninstall_tetherd_service(config_);

    EXPECT_TRUE(removed.errors.empty());
    EXPECT_FALSE(removed.changed);
}

TEST_F(ServiceTest, ExecStartPointsAtTheAppImageWhenRunningFromOne) {
    const tether::testing::ScopedEnv appimage("APPIMAGE", std::string("/opt/landline.AppImage"));

    EXPECT_EQ(tether::tetherd_exec_command(), "/opt/landline.AppImage --daemon");
}

TEST_F(ServiceTest, WhichProgramFindsOnlyWhatIsExecutable) {
    const fs::path bin = root_ / "bin";
    fs::create_directories(bin);
    std::ofstream(bin / "landline-fake") << "#!/bin/sh\n";
    std::ofstream(bin / "landline-data") << "not a program\n";
    fs::permissions(bin / "landline-fake", fs::perms::owner_all);

    const tether::testing::ScopedEnv path("PATH", bin.string());

    EXPECT_EQ(tether::which_program("landline-fake"), (bin / "landline-fake").string());
    EXPECT_TRUE(tether::which_program("landline-data").empty());
    EXPECT_TRUE(tether::which_program("landline-missing").empty());
}
