// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Flameshot Contributors

#pragma once

#include <QString>
#include <QtGlobal>

// Clipboard and staging-file rules for a finalized recording.
//
// "Copy" has to hand the *file* to the desktop, not a screenshot: the paste
// target is a file manager, a chat window or a mail composer, and each of them
// reads a different flavour (see `utils/filemimedata.h`). The file also has to
// survive the overlay closing, so it lives in a private staging directory of its
// own, never in the folder the user saves videos in, and nothing here deletes it
// while the clipboard still offers it.
namespace Recording {
namespace Clipboard {

// How long a staged recording that the clipboard no longer offers is kept
// before a later recording may remove it. The paste can happen minutes later,
// and a crashed session must not leave files behind forever.
constexpr qint64 kStagingRetentionMs = 24LL * 60 * 60 * 1000;

// `QStandardPaths::TempLocation/flameshot-recordings`. Pure naming: nothing is
// created, validated or deleted by asking for the path.
QString stagingDirectory();

// Where the ownership marker lives inside a staging directory. Pure naming, like
// `stagingDirectory()`: nothing is created or verified by asking. Exposed because
// the marker is the durable on-disk contract (owner-only regular file, exact
// content including its version), and only `createOwnedStagingDirectory()` ever
// creates it -- a marker written any other way is not evidence.
QString stagingMarkerPath(const QString& directory);

// The staging directory, created if and only if it can be created or verified
// *securely*: a plain directory (never a symlink, never a file), owned by this
// user, with no permission for group or others, **and** carrying this module's
// durable ownership marker (see `createOwnedStagingDirectory`). An owner-only
// directory at a predictable name is safe, but it is not evidence that this
// module created it, so a pre-existing directory without the marker is never
// adopted, marked, repaired or swept.
//
// When the usual path cannot be used (an existing directory this module cannot
// prove it created, a planted symlink, a foreign owner), a fresh directory owned
// by construction is created instead, so a leftover folder cannot make Copy
// impossible. The returned path is that directory, or empty when nothing could
// be secured; `error` then carries the reason. When the usual path was skipped,
// `error` carries that reason too even though a directory was returned.
QString ensureStagingDirectory(QString* error = nullptr);

// The directory `ensureStagingDirectory()` resolved for this process, or an
// empty string before it was called. The encoder's file and the retention sweep
// both use this one, so they can never disagree about which directory is in use.
QString activeStagingDirectory();

// True when `path` is a plain directory owned by this user and closed to group
// and others. Anything else -- a symlink, a foreign directory, a world-writable
// one -- is not safe to write recording files into or to sweep.
bool isPrivateDirectory(const QString& path, QString* error = nullptr);

// True when `path` is private *and* carries this module's ownership marker: the
// only proof that this module created it, and the only thing that makes its
// contents eligible for retention. Privacy alone is never enough.
bool isOwnedStagingDirectory(const QString& path, QString* error = nullptr);

// Creates `path` as a module-owned staging directory: a fresh owner-only
// directory plus its ownership marker, both exclusive, so the marker exists
// before the directory is ever usable and an existing directory is never
// adopted or marked. Refuses any existing entry and removes only what it created
// itself when the marker cannot be written. This is the only way a directory
// becomes module-owned (`ensureStagingDirectory()` uses it).
bool createOwnedStagingDirectory(const QString& path, QString* error = nullptr);

// A unique path inside the verified staging directory. The file is not created,
// so the encoder can refuse to clobber anything. Empty when the staging
// directory cannot be secured.
QString stagingFile(const QString& suffix);

// Optional hand-off so the selection outlives *this process*.
//
// `QClipboard::setMimeData` makes this process the selection owner, and on X11
// the selection dies with it. The application therefore hands the path to the
// clipboard daemon, which keeps offering it after the overlay process is gone.
// The hook is a function pointer because the daemon is application code while
// this module is also built and tested on its own (no application objects).
using PersistHandler = bool (*)(const QString& path);

// Installs the hand-off. Null (the default) means "this process owns the
// selection and that is all": the copy still works, it just does not survive
// the process.
void setPersistHandler(PersistHandler handler);
PersistHandler persistHandler();

// Puts `path` on the clipboard as a file URI in every flavour a desktop file
// target is known to read, then offers it to the persist handler. Returns false
// when the file does not exist, when nothing can be offered, or when the local
// clipboard did not keep what was set.
bool copyFileUri(const QString& path);

// The staging file the clipboard currently offers, or an empty string once
// another application took the selection.
QString offeredFile();

// Drops this process' selection ownership. The file itself is left alone: a
// paste may still be in flight.
void releaseOwnership();

// Removes staged recordings in `directory` that are older than `retentionMs`
// and that the clipboard does not currently offer.
//
// Requires `isOwnedStagingDirectory(directory)`: a directory that is private but
// carries no ownership marker is refused outright, so a pre-existing owner-only
// directory (and every file a user keeps in it) is never touched -- privacy and
// the uid establish that a directory is safe to use, never that this module
// created it. Only regular, non-symlink files whose name starts with the staging
// prefix are considered, plus the encoder's own scratch folders. Module-owned
// directories with a unique suffix (used when an unowned directory occupied the
// usual name) are swept too when the usual path is passed, so a squatter cannot
// make retention leak. Returns the number of entries removed.
int sweepStaging(const QString& directory,
                 qint64 retentionMs = kStagingRetentionMs);

} // namespace Clipboard
} // namespace Recording
