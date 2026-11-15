// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "remote/remotestorage.h"

#include <QProgressDialog>
#include <QString>

namespace RemoteStorage {

class RemoteUploader;

// Progress and error reporting for one asynchronous upload. Both the screenshot
// save path and the recording upload action use this so the reported behaviour
// cannot drift apart.
class RemoteUploadDialog : public QProgressDialog
{
    Q_OBJECT
public:
    // Prompts for settings and secrets as needed, then starts the upload. Returns
    // without side effects when remote storage is disabled or the user cancels.
    static void start(QWidget* parent, const QString& localFile, MediaKind kind);

    ~RemoteUploadDialog() override;

private:
    RemoteUploadDialog(const QString& localFile,
                       MediaKind kind,
                       const Settings& settings,
                       const Credentials& credentials,
                       QWidget* parent);
    void begin();

    RemoteUploader* m_uploader;
    QString m_localFile;
    Settings m_settings;
    Credentials m_credentials;
    MediaKind m_kind;
};

}
