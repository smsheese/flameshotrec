// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Flameshot Contributors

#pragma once

namespace Recording::Clipboard {

// Installs the production hand-off for a copied recording.
//
// `copyFileUri()` makes *this* process the selection owner, and on X11 the
// selection dies with the process. The hand-off offers the same path to
// FlameshotDaemon, which keeps it on the clipboard afterwards -- without it the
// URI is lost the moment the overlay closes, which is exactly the bug this
// module exists for. It is a separate translation unit because it depends on
// application code (the daemon) while `recordingclipboard.cpp` must stay
// buildable without it, and the integration check installs the same function so
// the tested path cannot drift from the shipped one.
void installDaemonHandOff();

} // namespace Recording::Clipboard
