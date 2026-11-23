// SPDX-License-Identifier: GPL-2.0-or-later
// The one engine release this version of the extension installs, pinned
// here and nowhere else: no remote manifest is read. A new engine release
// means a new version of the extension with these lines changed.
//
// The archive holds landline-engine-<version>/engine/ (landlined,
// landlinectl, landline-le-params, lib/, locale/, LICENSE, VERSION) and
// landline-engine-<version>/system/ (landline-setup, the polkit policy, the
// user unit and the obex drop-in).
//
// setupSha256 and leParamsSha256 are the sums of system/landline-setup and
// engine/landline-le-params inside that archive. They are written into the
// script that runs as root (lib/engineInstall.js, bootstrapScript()): it
// copies those two files out of the home folder and runs them only if the
// copies have these very sums.

export const ENGINE_RELEASE = Object.freeze({
    version: '0.2.1',
    arch: 'x86_64',
    url: 'https://github.com/d101c/landline/releases/download/engine-v0.2.1/landline-engine-0.2.1-x86_64.tar.zst',
    // sha256sum of the published archive (6623933 bytes) and of the two
    // files of it that the system step runs as root.
    sha256: '6590473c7afbc8f9e2eadcf8cea916a6b52ee1c6157006dad98fc5b9c52d81fe',
    setupSha256: '4a65e24b2ebd4e59bd2c84a27164ab6b6aed480f18c19553bbd1ce7c5911e27e',
    leParamsSha256: '54f7763439a87a916ffa1c90ddc4d9eacab827b3b21238d16ecd0eb5330bb9b4',
});
