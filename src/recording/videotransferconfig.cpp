// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Flameshot Contributors
//
// Configuration adapter for the transfer facade.
//
// `VideoTransfer::configuredOptions()` reads the application configuration
// through `RemoteStorage::configuredSettings()`, which needs `ConfigHandler`.
// That makes it application code, not library code: keeping it in
// `flameshot-recording` gave the static library an undefined reference to a
// symbol that only exists in the executable, so any test linking only the
// libraries could not link at all. It lives here instead, is compiled straight
// into the `flameshot` target, and the core transfer library stays free of
// configuration globals (no stub, no test-only duplicate).
#include "videotransfer.h"
#include "remote/remotestoragestore.h"

VideoTransferOptions VideoTransfer::configuredOptions()
{
    const RemoteStorage::Settings settings = RemoteStorage::configuredSettings();
    VideoTransferOptions options;
    options.protocol = settings.connection.protocol == RemoteStorage::Protocol::FTP
                         ? VideoTransferOptions::Protocol::FTP
                         : VideoTransferOptions::Protocol::SFTP;
    // S3 and SSH cannot be expressed here; report the feature as unavailable so
    // a legacy caller never uploads over the wrong transport.
    options.enabled = settings.enabled &&
                      (settings.connection.protocol == RemoteStorage::Protocol::FTP ||
                       settings.connection.protocol == RemoteStorage::Protocol::SFTP);
    options.host = settings.connection.host;
    options.port = settings.connection.port;
    options.username = settings.connection.username;
    options.remoteDirectory = settings.connection.remoteDirectory;
    options.privateKeyPath = settings.connection.privateKeyPath;
    options.knownHostsPath = settings.connection.knownHostsPath;
    // The password is never read from the configuration file.
    return options;
}
