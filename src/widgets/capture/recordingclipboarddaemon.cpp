// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Flameshot Contributors

#include "recordingclipboarddaemon.h"

#include "core/flameshotdaemon.h"
#include "recordingclipboard.h"

namespace Recording::Clipboard {
namespace {

// The daemon keeps offering the file after this process is gone. A false result
// means "no daemon took it" (no session bus, or the daemon refused the path): the
// copy still works for as long as this process owns the selection, which is why
// this is a note and not an error.
bool handOffToDaemon(const QString& path)
{
    return FlameshotDaemon::copyFileToClipboard(path);
}

} // namespace

void installDaemonHandOff()
{
    setPersistHandler(&handOffToDaemon);
}

} // namespace Recording::Clipboard
