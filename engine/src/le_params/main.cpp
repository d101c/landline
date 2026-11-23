// landline-le-params: asks the controller for other connection parameters
// on the LE link to one phone, and does nothing else.
//
// The command (HCI LE Connection Update) needs CAP_NET_RAW. Rather than
// give that to the whole daemon, the system assistant installs this program
// root-owned with that one capability on its file; the daemon runs it once
// per LE link. What it does, and all it does:
//   - checks its arguments: a controller "hciN", an address, and parameters
//     within Apple's rules for accessories (Accessory Design Guidelines R32,
//     57.6: interval a multiple of 15 ms, supervision 6 to 18 s, interval x
//     (latency + 1) at most 6 s); anything else is refused before any socket
//     is opened;
//   - finds the LE link to that address in the controller's list (no link:
//     refused, nothing sent);
//   - sends LE Connection Update on it, once, and reads the answer;
//   - then, for at most <watch s> (6 h at most), reads what becomes of that
//     link's parameters, and stops when the link ends.
// It reads no input, no file and no environment variable, never ends a
// link, and sends no other command.
//
//   landline-le-params hciN AA:BB:CC:DD:EE:FF <min ms> <max ms> <latency> <timeout ms> [<wait ms> [<watch s>]]
//
// Prints one line per result, "outcome=applied status=0 handle=… interval_ms=…
// latency=… timeout_ms=…" first, then "outcome=changed …" for each change
// seen while watching, and "outcome=ended status=<reason> …" at the end of
// the link. Exits 0 when the request was made (applied, refused, no
// verdict), 1 when it was not (no link, no right, failure), 2 on misuse.

#include "tether/bluetooth/le_conn_params.hpp"
#include "tether/bluetooth/le_conn_update.hpp"

#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>

using namespace tether::bluetooth;

namespace {

    constexpr int WAIT_MS_MAX = 30000;

    std::optional<int> number(const char* text) {
        if (!text || !*text || std::strlen(text) > 6)
            return std::nullopt;
        int value = 0;
        for (const char* c = text; *c; ++c) {
            if (*c < '0' || *c > '9')
                return std::nullopt;
            value = value * 10 + (*c - '0');
        }
        return value;
    }

    // Exactly "AA:BB:CC:DD:EE:FF", hexadecimal.
    bool address_like(const char* text) {
        if (!text || std::strlen(text) != 17)
            return false;
        for (int i = 0; i < 17; ++i) {
            const char c = text[i];
            if (i % 3 == 2) {
                if (c != ':')
                    return false;
            } else if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f'))) {
                return false;
            }
        }
        return true;
    }

    int misuse(const char* why) {
        std::fprintf(stderr,
                     "landline-le-params: %s\n"
                     "usage: landline-le-params hciN AA:BB:CC:DD:EE:FF <min ms> <max ms> <latency> <timeout ms> "
                     "[<wait ms> [<watch s>]]\n",
                     why);
        return 2;
    }

} // namespace

int main(int argc, char** argv) {
    if (argc < 7 || argc > 9)
        return misuse("six to eight arguments are expected");
    const auto index = hci_index_from(argv[1]);
    const auto min = number(argv[3]);
    const auto max = number(argv[4]);
    const auto latency = number(argv[5]);
    const auto timeout = number(argv[6]);
    const auto wait = argc >= 8 ? number(argv[7]) : std::optional<int>(LE_CONN_PARAMS_VERDICT_MS);
    const auto watch = argc >= 9 ? number(argv[8]) : std::optional<int>(0);
    if (!index || !min || !max || !latency || !timeout || !wait || !watch)
        return misuse("a value is not a number, or the controller is not hciN");
    if (!address_like(argv[2]))
        return misuse("the address is not AA:BB:CC:DD:EE:FF");
    if (*wait > WAIT_MS_MAX || *watch > LE_CONN_PARAMS_WATCH_SECONDS)
        return misuse("the wait is over 30 s or the watch over 6 h");

    LeConnParams params;
    params.min_interval_ms = *min;
    params.max_interval_ms = *max;
    params.latency = *latency;
    params.timeout_ms = *timeout;
    if (const std::string fault = le_conn_params_fault(params); !fault.empty())
        return misuse(fault.c_str());

    std::optional<LeConnUpdateOutcome> verdict;
    hci_le_conn_update(*index, argv[2], params, *wait, *watch, [&verdict](const LeConnUpdateResult& result) {
        if (!verdict)
            verdict = result.outcome;
        std::printf("%s\n", format_le_conn_update_result(result).c_str());
        // The daemon reads each line as it comes.
        std::fflush(stdout);
        return true;
    });
    return verdict == LeConnUpdateOutcome::Applied || verdict == LeConnUpdateOutcome::Refused ||
                   verdict == LeConnUpdateOutcome::NoVerdict
               ? 0
               : 1;
}
