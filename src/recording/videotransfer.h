// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QObject>
#include <QString>

// Backwards-compatible seam over RemoteStorage::RemoteUploader.
//
// The public surface is unchanged on purpose: the recording upload action and
// the existing tests call VideoTransfer::upload() with VideoTransferOptions and
// rely on these signals. The transfer itself is the shared implementation in
// src/remote/, which also supports SSH and S3. New code should call
// RemoteStorage::RemoteUploadDialog::start() or RemoteStorage::RemoteUploader
// directly; see /tmp/flameshot-next-notes/remote.md for the hookup.

struct VideoTransferOptions
{
    enum class Protocol { FTP, SFTP };
    bool enabled = false;
    Protocol protocol = Protocol::SFTP;
    QString host;
    quint16 port = 0;
    QString username;
    QString password; // FTP only; kept out of process arguments and logs.
    QString remoteDirectory;
    QString privateKeyPath; // SFTP public-key authentication.
    QString knownHostsPath; // Required for SFTP host verification.
};

// Asynchronously transfers a completed recording. The original file is never
// removed by this class, including after successful uploads.
class VideoTransfer : public QObject
{
    Q_OBJECT
public:
    explicit VideoTransfer(QObject* parent = nullptr);
    ~VideoTransfer() override;
    bool upload(const QString& finalizedLocalFile, const VideoTransferOptions& options);
    void cancel();
    bool isRunning() const;
    static QString curlExecutable();
    static VideoTransferOptions configuredOptions();

signals:
    void succeeded(const QString& localFile);
    void failed(const QString& message);

private:
    class Private;
    Private* d;
};
