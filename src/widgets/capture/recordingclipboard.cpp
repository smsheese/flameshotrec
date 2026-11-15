// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Flameshot Contributors

#include "recordingclipboard.h"

#include "utils/filemimedata.h"

#include <QClipboard>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QMimeData>
#include <QRect>
#include <QRegion>
#include <QSet>
#include <QStandardPaths>
#include <QStringList>
#include <QUrl>
#include <QUuid>
#include <QtDebug>

#if defined(Q_OS_UNIX)
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#endif

namespace Recording {
namespace Clipboard {
namespace {

// Every staged file starts with this prefix and every encoder scratch folder
// starts with the second one. The prefix is *not* what makes a file ours -- the
// private, verified, module-created directory with its ownership marker is -- it
// only narrows what the sweep looks at.
const char kStagingPrefix[] = "flameshot-recording-";
const char kEncoderPrefix[] = ".flameshot-recording-";
const char kStagingFolder[] = "flameshot-recordings";
// Durable evidence that this module created the directory. Kept out of both
// prefixes, so neither the file sweep nor the scratch-folder sweep can match it.
const char kStagingMarker[] = ".flameshot-staging-owner";
const char kStagingMarkerMagic[] = "flameshot-recording-staging 1";

// A sweep walks at most this many entries and this deep: retention must never
// become a way to make the overlay hang on a directory it does not expect.
constexpr int kMaxSweepEntries = 256;
constexpr int kMaxSweepDepth = 3;

// What this process currently offers. `QClipboard::setMimeData` transfers
// ownership of the data to the clipboard, which keeps it alive for as long as
// this process runs; the record here is what the retention sweep has to spare
// and what `offeredFile()` reports back to the caller.
struct Ownership
{
    QString path;
    bool connected{false};
};
Ownership& ownership()
{
    static Ownership state;
    return state;
}

PersistHandler& persistSlot()
{
    static PersistHandler handler = nullptr;
    return handler;
}

void setError(QString* error, const QString& reason)
{
    if (error != nullptr) {
        *error = reason;
    }
}

#if defined(Q_OS_UNIX)
// A non-following look at one path: a symlink is "other", never a directory,
// which is what makes a planted symlink useless to this module.
enum class Kind
{
    Missing,
    Directory,
    Other
};

Kind kindOf(const QString& path)
{
    const QByteArray native = QFile::encodeName(path);
    struct stat info
    {
    };
    if (::lstat(native.constData(), &info) != 0) {
        return errno == ENOENT ? Kind::Missing : Kind::Other;
    }
    return S_ISDIR(info.st_mode) ? Kind::Directory : Kind::Other;
}
#endif

// Removes one staged file. Never follows a link, never touches a directory.
bool removeStagedFile(const QString& path)
{
    const QFileInfo info(path);
    if (info.isSymLink() || !info.isFile()) {
        return false;
    }
    return QFile::remove(path);
}

// Removes one of the encoder's own abandoned scratch folders, but only when
// nothing inside it could still be in use and nothing inside it is a link.
bool removeAbandonedFolder(const QString& path, const QDateTime& cutoff)
{
    const QFileInfo info(path);
    if (info.isSymLink() || !info.isDir()) {
        return false;
    }
    QDir dir(path);
    const auto entries =
      dir.entryInfoList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot,
                        QDir::Name);
    if (entries.size() > kMaxSweepEntries) {
        return false;
    }
    for (const auto& entry : entries) {
        if (entry.isSymLink()) {
            // A link inside a scratch folder means this is not a folder this
            // module created; leave the whole tree alone.
            return false;
        }
        if (entry.lastModified().toUTC() > cutoff) {
            // Something was written here recently: it may still be a running
            // encoder, so nothing is removed.
            return false;
        }
    }
    if (entries.isEmpty() && info.lastModified().toUTC() > cutoff) {
        return false;
    }
    return dir.removeRecursively();
}

// Every path the sweep has to spare. Two sources: what this process put on the
// clipboard, and what the clipboard offers right now -- the second one covers a
// selection the clipboard daemon hosts after this process stopped owning it, or
// one another instance of Flameshot left behind.
QSet<QString> protectedPaths()
{
    QSet<QString> paths;
    if (!ownership().path.isEmpty()) {
        paths.insert(ownership().path);
    }
    QClipboard* clipboard = QGuiApplication::clipboard();
    if (clipboard == nullptr) {
        return paths;
    }
    const QMimeData* data = clipboard->mimeData();
    if (data == nullptr) {
        return paths;
    }
    for (const QUrl& url : data->urls()) {
        const QString local = url.toLocalFile();
        if (!local.isEmpty()) {
            paths.insert(QFileInfo(local).absoluteFilePath());
        }
    }
    if (!data->text().isEmpty() && QFileInfo(data->text()).isAbsolute()) {
        paths.insert(QFileInfo(data->text()).absoluteFilePath());
    }
    return paths;
}

// Called when any application replaces the selection: from that moment the
// staged file is no longer protected by this process' clipboard ownership, only
// by the retention window.
void forgetIfNotOwner()
{
    QClipboard* clipboard = QGuiApplication::clipboard();
    if (clipboard == nullptr) {
        return;
    }
    const QString path = ownership().path;
    if (path.isEmpty()) {
        return;
    }
    // Asked of the clipboard, not of the pointer: a daemon that took the
    // selection over still offers the same file, and dropping our record then
    // would make the file sweepable while the desktop still offers it.
    if (!protectedPaths().contains(path)) {
        ownership().path.clear();
    }
}

void ensureWatching()
{
    if (ownership().connected || QGuiApplication::clipboard() == nullptr) {
        return;
    }
    QObject::connect(QGuiApplication::clipboard(),
                     &QClipboard::dataChanged,
                     QGuiApplication::clipboard(),
                     []() { forgetIfNotOwner(); });
    ownership().connected = true;
}

// ---------------------------------------------------------------- ownership --

// What `createOwnedStagingDirectory()` writes and what everything else verifies.
// A directory is module-owned only when this file exists and matches exactly;
// privacy (0700, same uid) is a precondition of a safe directory, not evidence
// that this module created it. The version number is part of the line, so a
// future format change cannot be mistaken for today's.
QString markerContent()
{
#if defined(Q_OS_UNIX)
    const qulonglong uid = qulonglong(::geteuid());
#else
    const qulonglong uid = 0;
#endif
    return QStringLiteral("%1\nuid=%2\n")
      .arg(QString::fromLatin1(kStagingMarkerMagic))
      .arg(uid);
}

QString markerPath(const QString& directory)
{
    return QDir(directory).filePath(QString::fromLatin1(kStagingMarker));
}

// Creates the marker exclusively, inside a directory this module just created.
// O_EXCL refuses an existing entry, O_NOFOLLOW refuses to write through a link,
// and 0600 is the creation mode rather than a later, ignorable chmod.
bool writeOwnershipMarker(const QString& directory, QString* error)
{
    const QString path = markerPath(directory);
    const QByteArray content = markerContent().toUtf8();
#if defined(Q_OS_UNIX)
    const QByteArray native = QFile::encodeName(path);
    const int descriptor =
      ::open(native.constData(),
             O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
             0600);
    if (descriptor < 0) {
        setError(error,
                 QStringLiteral("the ownership marker %1 cannot be created: %2")
                   .arg(path, QString::fromLocal8Bit(std::strerror(errno))));
        return false;
    }
    qint64 written = 0;
    while (written < content.size()) {
        const ssize_t step = ::write(descriptor,
                                     content.constData() + written,
                                     size_t(content.size() - written));
        if (step <= 0) {
            if (step < 0 && errno == EINTR) {
                continue;
            }
            const int failure = errno;
            ::close(descriptor);
            ::unlink(native.constData());
            setError(error,
                     QStringLiteral("the ownership marker %1 cannot be written: "
                                    "%2")
                       .arg(path, QString::fromLocal8Bit(std::strerror(failure))));
            return false;
        }
        written += step;
    }
    // Durable evidence: the marker must be on disk before the directory is used,
    // otherwise a crash could leave a directory that is neither ours nor the
    // user's -- and it must never be adopted later.
    if (::fsync(descriptor) != 0) {
        const int failure = errno;
        ::close(descriptor);
        ::unlink(native.constData());
        setError(error,
                 QStringLiteral("the ownership marker %1 cannot be flushed: %2")
                   .arg(path, QString::fromLocal8Bit(std::strerror(failure))));
        return false;
    }
    ::close(descriptor);
    return true;
#else
    QFile marker(path);
    if (!marker.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
        setError(error,
                 QStringLiteral("the ownership marker %1 cannot be created")
                   .arg(path));
        return false;
    }
    marker.write(content);
    marker.close();
    if (!QFile::setPermissions(path,
                               QFile::ReadOwner | QFile::WriteOwner)) {
        QFile::remove(path);
        setError(error,
                 QStringLiteral("the ownership marker %1 is not private")
                   .arg(path));
        return false;
    }
    return true;
#endif
}

// Verifies the marker itself: a plain regular file (never a link), owned by this
// user, closed to group and others, of this version's exact content. Anything
// else means the directory is not this module's.
bool verifyOwnershipMarker(const QString& directory, QString* error)
{
    const QString path = markerPath(directory);
#if defined(Q_OS_UNIX)
    const QByteArray native = QFile::encodeName(path);
    struct stat listed
    {
    };
    if (::lstat(native.constData(), &listed) != 0) {
        setError(error,
                 QStringLiteral("%1 carries no ownership marker, so this module "
                                "did not create it")
                   .arg(directory));
        return false;
    }
    if (!S_ISREG(listed.st_mode)) {
        setError(error,
                 QStringLiteral("%1: the ownership marker is not a plain file")
                   .arg(directory));
        return false;
    }
    if (listed.st_uid != ::geteuid()) {
        setError(error,
                 QStringLiteral("%1: the ownership marker belongs to another "
                                "user (uid %2)")
                   .arg(directory)
                   .arg(qulonglong(listed.st_uid)));
        return false;
    }
    if ((listed.st_mode & (S_IRWXG | S_IRWXO)) != 0) {
        setError(error,
                 QStringLiteral("%1: the ownership marker is accessible to group "
                                "or others (mode %2)")
                   .arg(directory)
                   .arg(qulonglong(listed.st_mode & 07777), 0, 8));
        return false;
    }
    if (listed.st_size <= 0 || listed.st_size > 256) {
        setError(error,
                 QStringLiteral("%1: the ownership marker has an unexpected size "
                                "(%2 bytes)")
                   .arg(directory)
                   .arg(qulonglong(listed.st_size)));
        return false;
    }
    const int descriptor = ::open(native.constData(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (descriptor < 0) {
        setError(error,
                 QStringLiteral("%1: the ownership marker cannot be read: %2")
                   .arg(directory, QString::fromLocal8Bit(std::strerror(errno))));
        return false;
    }
    struct stat opened
    {
    };
    char buffer[257] = {};
    const ssize_t count = ::read(descriptor, buffer, 256);
    const bool same = ::fstat(descriptor, &opened) == 0 &&
                      opened.st_ino == listed.st_ino &&
                      opened.st_dev == listed.st_dev;
    ::close(descriptor);
    if (!same) {
        // The marker changed between the look and the open: not this module's.
        setError(error,
                 QStringLiteral("%1: the ownership marker changed while it was "
                                "being verified")
                   .arg(directory));
        return false;
    }
    if (count <= 0) {
        setError(error,
                 QStringLiteral("%1: the ownership marker is empty")
                   .arg(directory));
        return false;
    }
    if (QByteArray(buffer, int(count)) != markerContent().toUtf8()) {
        setError(error,
                 QStringLiteral("%1: the ownership marker has unexpected "
                                "content")
                   .arg(directory));
        return false;
    }
    return true;
#else
    const QFileInfo info(path);
    if (info.isSymLink() || !info.isFile()) {
        setError(error,
                 QStringLiteral("%1 carries no ownership marker")
                   .arg(directory));
        return false;
    }
    if (info.permissions() != (QFile::ReadOwner | QFile::WriteOwner)) {
        setError(error,
                 QStringLiteral("%1: the ownership marker is not private")
                   .arg(directory));
        return false;
    }
    QFile marker(path);
    if (!marker.open(QIODevice::ReadOnly) ||
        marker.read(257) != markerContent().toUtf8()) {
        setError(error,
                 QStringLiteral("%1: the ownership marker has unexpected "
                                "content")
                   .arg(directory));
        return false;
    }
    return true;
#endif
}

// A fresh name next to the usual staging directory, used only when the usual one
// cannot be proven to be this module's. `createOwnedStagingDirectory()` is what
// makes the name exclusive.
QString uniqueStagingDirectory()
{
    const QString base =
      QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    if (base.isEmpty()) {
        return {};
    }
    for (int attempt = 0; attempt < 16; ++attempt) {
        const QString candidate =
          QDir(base).filePath(QString::fromLatin1(kStagingFolder) +
                              QLatin1Char('-') +
                              QUuid::createUuid()
                                .toString(QUuid::WithoutBraces)
                                .left(12));
        if (kindOf(candidate) == Kind::Missing) {
            return candidate;
        }
    }
    return {};
}

// Resolution of the staging directory for this process: computed once, so
// `stagingFile()` and the retention sweep can never disagree about which
// directory is in use.
struct StagingResolution
{
    bool resolved{false};
    QString directory;
    // Why the usual location was not used, for the caller's message.
    QString refusal;
};
StagingResolution& stagingResolution()
{
    static StagingResolution state;
    return state;
}

} // namespace

QString stagingDirectory()
{
    const QString base =
      QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    if (base.isEmpty()) {
        return {};
    }
    return QDir(base).filePath(QString::fromLatin1(kStagingFolder));
}

QString stagingMarkerPath(const QString& directory)
{
    if (directory.isEmpty()) {
        return {};
    }
    return markerPath(directory);
}

bool isPrivateDirectory(const QString& path, QString* error)
{
    if (path.isEmpty()) {
        setError(error, QStringLiteral("empty path"));
        return false;
    }
#if defined(Q_OS_UNIX)
    const QByteArray native = QFile::encodeName(path);
    struct stat info
    {
    };
    if (::lstat(native.constData(), &info) != 0) {
        setError(error,
                 QStringLiteral("%1 cannot be inspected: %2")
                   .arg(path, QString::fromLocal8Bit(std::strerror(errno))));
        return false;
    }
    if (!S_ISDIR(info.st_mode)) {
        // A symlink to a private directory would pass every later check while
        // pointing somewhere else, so it is refused here rather than resolved.
        setError(error,
                 QStringLiteral("%1 is not a plain directory (a symlink, a file "
                                "or a device)")
                   .arg(path));
        return false;
    }
    if (info.st_uid != ::geteuid()) {
        setError(error,
                 QStringLiteral("%1 belongs to another user (uid %2)")
                   .arg(path)
                   .arg(qulonglong(info.st_uid)));
        return false;
    }
    if ((info.st_mode & (S_IRWXG | S_IRWXO)) != 0) {
        setError(error,
                 QStringLiteral("%1 is accessible to group or others (mode %2)")
                   .arg(path)
                   .arg(qulonglong(info.st_mode & 07777), 0, 8));
        return false;
    }
    return true;
#else
    const QFileInfo info(path);
    if (info.isSymLink() || !info.isDir()) {
        setError(error,
                 QStringLiteral("%1 is not a plain directory").arg(path));
        return false;
    }
    if (info.permissions() !=
        (QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner)) {
        setError(error,
                 QStringLiteral("%1 is not closed to group and others")
                   .arg(path));
        return false;
    }
    return true;
#endif
}

QString ensureStagingDirectory(QString* error)
{
    StagingResolution& resolution = stagingResolution();
    if (resolution.resolved) {
        // One answer per process: the name handed to the encoder and the
        // directory retention sweeps can never disagree.
        setError(error, resolution.refusal);
        return resolution.directory;
    }
    resolution.resolved = true;
    const QString canonical = stagingDirectory();
    if (canonical.isEmpty()) {
        resolution.refusal =
          QStringLiteral("no temporary location is available");
        setError(error, resolution.refusal);
        return {};
    }
    // A private directory is only *usable* when it also carries this module's
    // ownership marker. Privacy and ownership of the uid establish that the
    // directory is a safe place, never that this module created it.
    QString reason;
    if (isOwnedStagingDirectory(canonical, &reason)) {
        resolution.directory = canonical;
        return canonical;
    }
    // Only a name that does not exist yet may be created -- and creating it is
    // the only way a directory becomes module-owned.
    if (kindOf(canonical) == Kind::Missing &&
        createOwnedStagingDirectory(canonical, &reason)) {
        resolution.directory = canonical;
        return canonical;
    }
    // An existing directory that cannot be proven to be this module's is never
    // adopted, never marked, never repaired and never swept: it is the user's
    // until this module can show otherwise. A fresh directory owned by
    // construction is used instead, so a leftover folder with the same name
    // cannot make Copy impossible.
    const QString fallback = uniqueStagingDirectory();
    QString fallbackReason;
    if (fallback.isEmpty() ||
        !createOwnedStagingDirectory(fallback, &fallbackReason)) {
        resolution.refusal = reason.isEmpty() ? fallbackReason : reason;
        if (resolution.refusal.isEmpty()) {
            resolution.refusal =
              QStringLiteral("no module-owned staging directory could be "
                             "created in %1")
                .arg(QFileInfo(canonical).absolutePath());
        }
        setError(error, resolution.refusal);
        return {};
    }
    resolution.directory = fallback;
    resolution.refusal = reason; // why the usual location was not used
    setError(error, reason);
    return fallback;
}

QString activeStagingDirectory()
{
    return stagingResolution().directory;
}

bool isOwnedStagingDirectory(const QString& path, QString* error)
{
    if (!isPrivateDirectory(path, error)) {
        return false;
    }
    return verifyOwnershipMarker(path, error);
}

bool createOwnedStagingDirectory(const QString& path, QString* error)
{
    if (path.isEmpty()) {
        setError(error, QStringLiteral("empty path"));
        return false;
    }
#if defined(Q_OS_UNIX)
    const QByteArray native = QFile::encodeName(path);
    // mkdir() with the mode: it refuses an entry that appeared in the meantime
    // (a planted symlink gives EEXIST instead of being followed) and the mode is
    // part of the creation rather than a later, ignorable permission call.
    if (::mkdir(native.constData(), 0700) != 0) {
        setError(error,
                 QStringLiteral("%1 cannot be created: %2")
                   .arg(path, QString::fromLocal8Bit(std::strerror(errno))));
        return false;
    }
#else
    if (!QDir().mkdir(path)) {
        setError(error, QStringLiteral("%1 cannot be created").arg(path));
        return false;
    }
    QFile::setPermissions(path,
                          QFile::ReadOwner | QFile::WriteOwner |
                            QFile::ExeOwner);
#endif
    // The marker is written *now*, inside a directory this call created, so
    // there is no window in which the directory is usable but unowned: another
    // process that sees it before the marker fails closed and uses its own
    // directory instead of adopting a half-initialised one.
    bool marked = writeOwnershipMarker(path, error);
    if (marked) {
        marked = isOwnedStagingDirectory(path, error);
    }
    if (!marked) {
        // Undo only what this call created: rmdir() refuses a non-empty
        // directory, so nothing anybody else put there can be removed.
        QFile::remove(markerPath(path));
#if defined(Q_OS_UNIX)
        ::rmdir(native.constData());
#else
        QDir().rmdir(path);
#endif
        return false;
    }
    return true;
}

QString stagingFile(const QString& suffix)
{
    const QString directory = ensureStagingDirectory();
    if (directory.isEmpty()) {
        return {};
    }
    const QString name = QString::fromLatin1(kStagingPrefix) +
                         QUuid::createUuid().toString(QUuid::WithoutBraces) +
                         QLatin1Char('.') + suffix;
    return QDir(directory).filePath(name);
}

void setPersistHandler(PersistHandler handler)
{
    persistSlot() = handler;
}

PersistHandler persistHandler()
{
    return persistSlot();
}

bool copyFileUri(const QString& path)
{
    QClipboard* clipboard = QGuiApplication::clipboard();
    QMimeData* data = FileMimeData::forLocalFile(path);
    if (clipboard == nullptr || data == nullptr) {
        delete data;
        return false;
    }
    const QString absolute = QFileInfo(path).absoluteFilePath();
    clipboard->setMimeData(data);
    ensureWatching();
    ownership().path = absolute;
    // Confirm the flavours really made it to the clipboard: a mismatched
    // dataChanged (another manager taking over immediately) must not leave the
    // caller believing the file is offered.
    const QMimeData* stored = clipboard->mimeData();
    const bool local = stored != nullptr && stored->hasUrls() &&
                       stored->urls().value(0).toLocalFile() == absolute;
    if (!local) {
        return false;
    }
    // Hand the path to the clipboard daemon so the selection outlives this
    // process. Best effort by design: without a daemon (or without a session
    // bus) the copy still works for as long as this process owns the selection,
    // which is why a failure here is a note and not an error.
    const PersistHandler handler = persistSlot();
    if (handler != nullptr && !handler(absolute)) {
        qInfo() << "recording clipboard: no clipboard daemon took the file URI;"
                << "the selection is held by this process:"
                << absolute;
    }
    return true;
}

QString offeredFile()
{
    return ownership().path;
}

void releaseOwnership()
{
    ownership().path.clear();
}

// The sweep itself, for a directory already proven to be module-owned (private
// *and* carrying this module's marker). Split out so the same rules apply to the
// usual staging directory and to a unique one that had to be used instead.
int sweepOwnedDirectory(const QString& directory, qint64 retentionMs)
{
    const QDir dir(directory);
    const QDateTime cutoff =
      QDateTime::currentDateTimeUtc().addMSecs(-retentionMs);
    const QSet<QString> protectedFiles = protectedPaths();
    int removed = 0;
    int inspected = 0;

    const QStringList files =
      dir.entryList(QDir::Files | QDir::Hidden | QDir::NoSymLinks, QDir::Name);
    for (const QString& name : files) {
        if (++inspected > kMaxSweepEntries) {
            break;
        }
        if (!name.startsWith(QString::fromLatin1(kStagingPrefix))) {
            continue;
        }
        const QFileInfo file(dir.filePath(name));
        // Never touch a symlink, a directory, the file the clipboard still
        // offers, or anything inside the retention window.
        if (file.isSymLink() || !file.isFile()) {
            continue;
        }
        if (protectedFiles.contains(file.absoluteFilePath())) {
            continue;
        }
        if (file.lastModified().toUTC() > cutoff) {
            continue;
        }
        if (removeStagedFile(file.absoluteFilePath())) {
            ++removed;
        }
    }

    // The encoder's own scratch folders, which a crash can leave behind. They
    // are only candidates inside this private directory, only under the encoder
    // prefix, and only when every entry inside is past the retention window.
    const QStringList folders =
      dir.entryList(QDir::Dirs | QDir::Hidden | QDir::NoSymLinks, QDir::Name);
    for (const QString& name : folders) {
        if (++inspected > kMaxSweepEntries) {
            break;
        }
        if (!name.startsWith(QString::fromLatin1(kEncoderPrefix))) {
            continue;
        }
        const QFileInfo folder(dir.filePath(name));
        if (folder.isSymLink() || !folder.isDir()) {
            continue;
        }
        // The folder's own timestamp is not the question: an encoder writes
        // *inside* it, so `removeAbandonedFolder` decides on the entries.
        if (removeAbandonedFolder(folder.absoluteFilePath(), cutoff)) {
            ++removed;
        }
    }
    return removed;
}

int sweepStaging(const QString& directory, qint64 retentionMs)
{
    if (retentionMs < 0 || directory.isEmpty()) {
        return 0;
    }
    int removed = 0;
    QString reason;
    if (isOwnedStagingDirectory(directory, &reason)) {
        removed += sweepOwnedDirectory(directory, retentionMs);
    } else {
        // Never sweep a directory this module did not create: privacy and the
        // uid say the directory is safe, a prefix says nothing, and an existing
        // owner-only directory at a predictable name is the user's until this
        // module can show it created it. The configured video folder, a legacy
        // staging folder and a foreign directory all take this branch.
        qWarning() << "recording retention: refusing to sweep" << directory
                   << "-" << reason;
    }
    // A module-owned directory can also carry a unique suffix, which is what
    // happens when an unowned directory occupies the usual name. Those are
    // marked as well, so they are collected here: a squatter must not be able to
    // make retention leak forever, and nothing unmarked is ever considered.
    if (QFileInfo(directory).absoluteFilePath() ==
        QFileInfo(stagingDirectory()).absoluteFilePath()) {
        const QFileInfo canonical(directory);
        QDir parent(canonical.absolutePath());
        const QStringList siblings = parent.entryList(
          QDir::Dirs | QDir::Hidden | QDir::NoSymLinks, QDir::Name);
        int inspected = 0;
        for (const QString& name : siblings) {
            if (++inspected > kMaxSweepEntries) {
                break;
            }
            if (!name.startsWith(QString::fromLatin1(kStagingFolder) +
                                 QLatin1Char('-')) ||
                name == canonical.fileName()) {
                continue;
            }
            const QString sibling = parent.filePath(name);
            QString siblingReason;
            if (!isOwnedStagingDirectory(sibling, &siblingReason)) {
                continue;
            }
            removed += sweepOwnedDirectory(sibling, retentionMs);
            // A directory that only held its own marker is finished: rmdir()
            // refuses anything else, so this can never remove content.
            parent.rmdir(name);
        }
    }
    return removed;
}

} // namespace Clipboard
} // namespace Recording
