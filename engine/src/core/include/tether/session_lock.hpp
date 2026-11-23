#pragma once

#include <string>

namespace tether {

    // Locks the desktop session. With `command` empty this asks logind.
    bool lock_session(const std::string& command);

    // Lights up the login screen of a locked session by running `command`.
    // Never unlocks. With `command` empty nothing is run and this is false:
    // no desktop offers a call for it that would not also dismiss the lock.
    bool wake_session(const std::string& command);

    // Locked or idle: logind's LockedHint or IdleHint (GNOME has no
    // ext-idle-notify; gnome-session sets IdleHint). False when logind cannot
    // be asked.
    bool session_inactive();

} // namespace tether
