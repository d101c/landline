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
    version: '0.3.0',
    arch: 'x86_64',
    url: 'https://github.com/d101c/landline/releases/download/engine-v0.3.0/landline-engine-0.3.0-x86_64.tar.zst',
    // sha256sum of the published archive (6642026 bytes) and of the two
    // files of it that the system step runs as root.
    sha256: 'd1b0b997b979a5218559cd8f34ea211f084d79e9cf8e694c43168129478d02d7',
    setupSha256: '4a65e24b2ebd4e59bd2c84a27164ab6b6aed480f18c19553bbd1ce7c5911e27e',
    leParamsSha256: '0a09cb0f97c734ad0d1a151fc0aafd1abd93836d091c913ccf8c9cc8519d3719',
});
