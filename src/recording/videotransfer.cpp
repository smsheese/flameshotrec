// SPDX-License-Identifier: GPL-3.0-or-later
#include "videotransfer.h"
#include "remote/remotestorage.h"
#include "remote/remoteuploader.h"

#include <QFileInfo>

namespace {

RemoteStorage::Settings toSettings(const VideoTransferOptions& options)
{
    RemoteStorage::Settings settings;
    settings.enabled = options.enabled;
    RemoteStorage::Connection& connection = settings.connection;
    connection.protocol = options.protocol == VideoTransferOptions::Protocol::FTP
                            ? RemoteStorage::Protocol::FTP
                            : RemoteStorage::Protocol::SFTP;
    connection.host = options.host;
    connection.port = options.port;
    connection.username = options.username;
    connection.remoteDirectory = options.remoteDirectory;
    connection.privateKeyPath = options.privateKeyPath;
    connection.knownHostsPath = options.knownHostsPath;
    // The legacy caller supplies the secret itself (it prompts before calling
    // upload), so no environment lookup or prompt happens inside the transfer.
    connection.secretSource = RemoteStorage::SecretSource::Prompt;
    settings.image.conversion = RemoteStorage::Conversion::Original;
    settings.video.conversion = RemoteStorage::Conversion::Original;
    return settings;
}

RemoteStorage::Credentials toCredentials(const VideoTransferOptions& options)
{
    RemoteStorage::Credentials credentials;
    credentials.password = options.password;
    return credentials;
}

} // namespace

class VideoTransfer::Private
{
public:
    RemoteStorage::RemoteUploader* uploader = nullptr;
    VideoTransfer* owner = nullptr;
};

VideoTransfer::VideoTransfer(QObject* parent)
  : QObject(parent)
  , d(new Private)
{
    d->owner = this;
    d->uploader = new RemoteStorage::RemoteUploader(this);
    connect(d->uploader, &RemoteStorage::RemoteUploader::succeeded, this,
            [this](const QString& localFile, const QString&) { emit succeeded(localFile); });
    connect(d->uploader, &RemoteStorage::RemoteUploader::failed, this,
            [this](const QString& message) { emit failed(message); });
}

VideoTransfer::~VideoTransfer()
{
    d->uploader->cancel();
    delete d;
}

bool VideoTransfer::upload(const QString& finalizedLocalFile,
                           const VideoTransferOptions& options)
{
    // A missing local file is reported before anything about the destination:
    // nothing can be sent whatever the settings say, and "the file is missing"
    // is the useful message at that point.
    const QFileInfo local(finalizedLocalFile);
    if (!local.exists() || !local.isFile() || local.size() <= 0) {
        emit failed(tr("The file to upload is missing or unreadable; nothing "
                       "was sent."));
        return false;
    }
    const RemoteStorage::Settings settings = toSettings(options);
    const QString settingsError =
      RemoteStorage::validate(settings, RemoteStorage::MediaKind::Video);
    if (!settingsError.isEmpty()) {
        emit failed(settingsError);
        return false;
    }
    return d->uploader->upload(finalizedLocalFile, RemoteStorage::MediaKind::Video,
                               settings, toCredentials(options));
}

void VideoTransfer::cancel()
{
    d->uploader->cancel();
}

bool VideoTransfer::isRunning() const
{
    return d->uploader->isRunning();
}

QString VideoTransfer::curlExecutable()
{
    return RemoteStorage::RemoteUploader::curlExecutable();
}

