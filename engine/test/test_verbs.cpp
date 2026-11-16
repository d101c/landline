#include "../src/cli/verbs.hpp"

#include <gtest/gtest.h>

namespace {

    std::vector<std::string> expand(std::initializer_list<const char*> args) {
        std::vector<std::string> in(args.begin(), args.end());
        return tether::cli::expand_verbs(in);
    }

} // namespace

TEST(Verbs, RewritesASubcommandIntoItsFlag) {
    EXPECT_EQ(expand({"landlinectl", "devices"}), (std::vector<std::string>{"landlinectl", "--list-devices"}));
    EXPECT_EQ(expand({"landlinectl", "paste"}), (std::vector<std::string>{"landlinectl", "--get-clipboard"}));
    EXPECT_EQ(expand({"landlinectl", "discover"}), (std::vector<std::string>{"landlinectl", "--discover"}));
    EXPECT_EQ(expand({"landlinectl", "share"}), (std::vector<std::string>{"landlinectl", "--share-clipboard"}));
}

TEST(Verbs, KeepsTheArgumentsThatFollow) {
    EXPECT_EQ(expand({"landlinectl", "accept", "9a4f21"}), (std::vector<std::string>{"landlinectl", "--accept", "9a4f21"}));
    EXPECT_EQ(expand({"landlinectl", "paste", "--host", "10.0.0.2"}),
              (std::vector<std::string>{"landlinectl", "--get-clipboard", "--host", "10.0.0.2"}));
}

TEST(Verbs, MapsBtToTheMatchingBluetoothFlag) {
    EXPECT_EQ(expand({"landlinectl", "bt", "setup"}), (std::vector<std::string>{"landlinectl", "--bt-setup"}));
    EXPECT_EQ(expand({"landlinectl", "bt", "pair", "AA:BB"}), (std::vector<std::string>{"landlinectl", "--bt-pair", "AA:BB"}));
    EXPECT_EQ(expand({"landlinectl", "bt", "airpods-mode", "anc"}),
              (std::vector<std::string>{"landlinectl", "--bt-airpods-mode", "anc"}));
}

TEST(Verbs, LeavesFlagsAlone) {
    const auto flags = expand({"landlinectl", "--bt-send", "thread", "hello"});
    EXPECT_EQ(flags, (std::vector<std::string>{"landlinectl", "--bt-send", "thread", "hello"}));

    // Value of a flag, not a subcommand: only the first argument is a verb.
    EXPECT_EQ(expand({"landlinectl", "-s", "paste"}), (std::vector<std::string>{"landlinectl", "-s", "paste"}));
}

TEST(Verbs, LeavesSomethingItDoesNotKnowForTheParserToReject) {
    EXPECT_EQ(expand({"landlinectl", "frobnicate"}), (std::vector<std::string>{"landlinectl", "frobnicate"}));
    EXPECT_EQ(expand({"landlinectl", "bt"}), (std::vector<std::string>{"landlinectl", "bt"}));
    EXPECT_EQ(expand({"landlinectl", "bt", "--help"}), (std::vector<std::string>{"landlinectl", "bt", "--help"}));
    EXPECT_EQ(expand({"landlinectl"}), (std::vector<std::string>{"landlinectl"}));
}

TEST(Verbs, EveryVerbHasAFlagAndHelp) {
    for (size_t i = 0; i < tether::cli::kVerbCount; ++i) {
        const auto& verb = tether::cli::kVerbs[i];
        EXPECT_NE(verb.verb[0], '-') << verb.verb;
        EXPECT_EQ(verb.flag[0], '-') << verb.verb;
        EXPECT_NE(verb.usage[0], '\0') << verb.verb;
    }
}
