#pragma once

// The names this engine goes by on a machine: programs, directories, files,
// keyring entry. The engine is derived from Tether (MIT, Zack Bartel); the C++
// namespace and the include directory keep that name, everything a user or
// another program can see is spelled here.
//
// Translated messages cannot be built from these (gettext wants literals), so
// they spell the names out. Environment variables read by the engine all start
// with LANDLINE_.
namespace tether::identity {

    inline constexpr const char* DISPLAY_NAME = "Landline";

    // Directory name under $XDG_CONFIG_HOME, $XDG_DATA_HOME, $XDG_STATE_HOME
    // and $XDG_RUNTIME_DIR.
    inline constexpr const char* APP_DIR = "landline";

    inline constexpr const char* DAEMON = "landlined";
    inline constexpr const char* CLI = "landlinectl";

    // Inside $XDG_RUNTIME_DIR/<APP_DIR>.
    inline constexpr const char* SOCKET_FILE = "landlined.sock";
    inline constexpr const char* LOCK_FILE = "landlined.lock";

    // Inside $XDG_STATE_HOME/<APP_DIR>.
    inline constexpr const char* LOG_FILE = "landlined.log";

    // The user unit the install script writes, and where enabling it links it.
    inline constexpr const char* ENGINE_UNIT = "landline-engine.service";
    inline constexpr const char* ENGINE_UNIT_WANTS = "graphical-session.target.wants";

    // The unit `landlinectl --install-service` writes for a bare engine.
    inline constexpr const char* DAEMON_UNIT = "landlined.service";

    // Keyring entry holding the key of the encrypted message store.
    inline constexpr const char* SECRET_SCHEMA = "io.github.d101c.landline.Store";
    inline constexpr const char* SECRET_APPLICATION = "landline";
    inline constexpr const char* SECRET_LABEL = "Landline message store key";

    // The entry an install under the engine's original name left behind. It is
    // read once, to adopt its key; it is never written or removed.
    inline constexpr const char* LEGACY_SECRET_SCHEMA = "com.tether.Store";
    inline constexpr const char* LEGACY_SECRET_APPLICATION = "tether";

} // namespace tether::identity
