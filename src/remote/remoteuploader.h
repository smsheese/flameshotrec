// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "remote/remotestorage.h"

#include <QByteArray>
#include <QDateTime>
#include <QObject>
#include <QScopedPointer>
#include <QString>
#include <QStringList>

class QProcess;
class QTemporaryDir;

namespace RemoteStorage {

// One step of a transfer. Building the steps is a pure function so the argument
// list, stdin payload and signed headers can be checked without starting a
// process or contacting a server.
struct Command
{
    QString program;
    QStringList arguments;
    QByteArray stdinData;  // curl configuration or sftp batch
    QString stdinFile;     // streamed into the process stdin instead (ssh)
    QString uploadFile;    // curl --upload-file target
    QString label;         // progress text ("Checking", "Transferring", ...)
    // Exit code that means "the destination already exists". Nothing has been
    // overwritten when this is returned.
    int collisionExitCode = -1;
    // Exit code that means "the bytes did not all arrive" (SSH byte check).
    int interruptedExitCode = -1;
    // The probe step succeeds only when stdout/stderr shows the target is absent.
    // FTP/SFTP no longer probe (see the plan builders); S3/SSH need no probe.
    bool probeMustBeAbsent = false;
    // The step's output must contain a positively parsed size record matching
    // expectSizeInOutput. "sftp" parses the ls -l record for that path, "ftp"
    // parses a 213 SIZE reply, "final" re-checks the published object.
    QByteArray sizeCheckKind; // "", "sftp", "ftp"
    // The rename step's exit status is not decisive: a follow-up size check on
    // the published object decides, so an FTP server that refuses RNFR/RNTO is
    // still caught instead of being reported as success.
    bool advisoryExitStatus = false;
    // Read the last stdout line as an HTTP status; only 2xx counts as success.
    bool expectOkHttpStatus = false;
    // Byte count the size check above must report.
    qint64 expectSizeInOutput = -1;
};

// A transfer is a short plan: optional existence probe, the upload itself, then
// the publish step. `transferId` names the per-transfer temporary file, so two
// transfers can never truncate each other's data.
QList<Command> buildTransferPlan(const Settings& settings,
                                 MediaKind kind,
                                 const QString& artifactPath,
                                 const QString& remoteName,
                                 const QString& mimeType,
                                 const Credentials& credentials,
                                 const QDateTime& now,
                                 const QString& transferId,
                                 QString* error);

// Best-effort removal of this transfer's temporary file after a failure. Returns
// an empty argument list when the transport cleans up after itself. It never
// touches the final object or another transfer's file.
bool buildCleanupCommand(const Settings& settings,
                         MediaKind kind,
                         const QString& remoteName,
                         const QString& transferId,
                         const Credentials& credentials,
                         Command* command);

// The remote location reported to the user. Contains no credentials.
QString remoteLocation(const Settings& settings,
                       MediaKind kind,
                       const QString& remoteName);

// FFmpeg argument list for the video profile. Pure function for the same
// reason: the tests assert the real transcode flags, not a rename.
bool buildVideoArguments(const VideoProfile& profile,
                         const QString& source,
                         const QString& target,
                         QStringList* arguments,
                         QString* error);

// Name of the temporary object this transfer owns. Visible for tests.
QString temporaryRemoteName(const QString& remoteName, const QString& transferId);

// Outcome of reading a remote size back. Only `Match` may publish: an
// unparsable listing is never treated as success.
enum class SizeCheck { Match, Mismatch, Unparsable };

// Parses the single `ls -l` record for `remotePath` and compares its size field.
// A number appearing anywhere else in the output is not evidence.
SizeCheck checkSftpListingSize(const QString& output,
                               const QString& remotePath,
                               qint64 expectedSize);

// Parses an FTP `213 <size>` reply.
SizeCheck checkFtpSizeReply(const QString& output, qint64 expectedSize);

// Final remote name for this upload: the base name gains a short unique suffix
// before its extension, so two uploads of the same screenshot or recording name
// become distinct objects instead of racing for one destination.
QString uniqueRemoteFileName(const QString& remoteName, const QString& transferId);

// Transport capability summary used by the UI to state guarantees honestly.
QString noReplaceGuarantee(Protocol protocol);

class RemoteUploader : public QObject
{
    Q_OBJECT
public:
    explicit RemoteUploader(QObject* parent = nullptr);
    ~RemoteUploader() override;

    // Starts an asynchronous upload. Returns false and emits failed() when the
    // settings or the local file are unusable. The local file is never removed.
    bool upload(const QString& localFile,
                MediaKind kind,
                const Settings& settings,
                const Credentials& credentials);
    void cancel();
    bool isRunning() const;

    static QString curlExecutable();
    static QString sftpExecutable();
    static QString sshExecutable();
    static QString ffmpegExecutable(const QString& configuredPath = {});

signals:
    void progress(const QString& message);
    void prepared(const QString& transferFile,
                  const QString& remoteName,
                  const QString& mimeType);
    void succeeded(const QString& localFile, const QString& remoteLocation);
    void failed(const QString& message);

private:
    void startTransfer();
    void startNextStep();
    void runCleanupThenFail(const QString& message);
    void startTranscode();
    void handleTranscodeOutput();
    void finishWithError(const QString& message);
    void finishSuccessfully();
    void cleanupProcess();

    QProcess* m_process = nullptr;
    QScopedPointer<QTemporaryDir> m_workspace;
    QString m_localFile;
    QString m_artifact;
    QString m_remoteName;
    QString m_mimeType;
    Settings m_settings;
    Credentials m_credentials;
    MediaKind m_kind = MediaKind::Image;
    bool m_transcoding = false;
    QByteArray m_progressBuffer;
    QList<Command> m_steps;
    int m_stepIndex = 0;
    QString m_transferId;
    QString m_pendingFailure;
    bool m_ownCatalog = false;
    bool m_cleaningUp = false;
    // Cancellation and completion are terminal: no later process result may
    // advance the plan or emit a second outcome.
    bool m_cancelled = false;
    bool m_terminal = false;
};

}
