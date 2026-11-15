// SPDX-License-Identifier: GPL-3.0-or-later
#include "remote/remoteuploaddialog.h"
#include "remote/remotestoragestore.h"
#include "remote/remoteuploader.h"

#include <QFileInfo>
#include <QMessageBox>

namespace RemoteStorage {

RemoteUploadDialog::RemoteUploadDialog(const QString& localFile,
                                       MediaKind kind,
                                       const Settings& settings,
                                       const Credentials& credentials,
                                       QWidget* parent)
  : QProgressDialog(parent)
  , m_uploader(new RemoteUploader(this))
  , m_localFile(localFile)
  , m_settings(settings)
  , m_credentials(credentials)
  , m_kind(kind)
{
    setWindowTitle(tr("Remote storage"));
    setLabelText(tr("Uploading %1…").arg(QFileInfo(localFile).fileName()));
    setRange(0, 0); // indeterminate: the transfer tools report no percentage
    setCancelButtonText(tr("Cancel"));
    setMinimumDuration(0);
    setAutoClose(false);
    setAutoReset(false);
    setAttribute(Qt::WA_DeleteOnClose);

    connect(this, &QProgressDialog::canceled, this, [this]() {
        m_uploader->cancel();
        m_credentials = Credentials{};
        close();
    });
    connect(m_uploader, &RemoteUploader::progress, this, [this](const QString& message) {
        setLabelText(tr("%1\nSaved file: %2").arg(message, m_localFile));
    });
    connect(m_uploader, &RemoteUploader::succeeded, this,
            [this](const QString& savedFile, const QString& location) {
                setValue(maximum());
                close();
                // Do not keep the secret for the lifetime of the dialog. QString
                // storage is implicitly shared, so this drops our reference; it
                // does not promise the bytes are erased.
                m_credentials = Credentials{};
                QMessageBox::information(
                  parentWidget(), tr("Upload finished"),
                  tr("Uploaded to %1.\nThe local file is kept at %2.")
                    .arg(location, savedFile));
            });
    connect(m_uploader, &RemoteUploader::failed, this, [this](const QString& message) {
        setValue(maximum());
        close();
        m_credentials = Credentials{};
        QMessageBox::warning(parentWidget(), tr("Upload failed"),
                             tr("%1\nThe local file is kept.").arg(message));
    });
}

RemoteUploadDialog::~RemoteUploadDialog()
{
    if (m_uploader != nullptr) {
        m_uploader->cancel();
    }
    m_credentials = Credentials{};
}

void RemoteUploadDialog::begin()
{
    show();
    m_uploader->upload(m_localFile, m_kind, m_settings, m_credentials);
    // The uploader holds its own copy for this transfer only.
    m_credentials = Credentials{};
}

void RemoteUploadDialog::start(QWidget* parent, const QString& localFile, MediaKind kind)
{
    const Settings settings = configuredSettings();
    if (!settings.enabled) {
        QMessageBox::information(
          parent, tr("Remote storage"),
          tr("Remote storage is disabled. Turn it on in Configuration > "
             "Remote Storage before uploading."));
        return;
    }
    Credentials credentials;
    bool accepted = true;
    QString error;
    if (!resolveCredentials(settings.connection, &credentials, parent, &accepted, &error)) {
        if (accepted) {
            QMessageBox::warning(parent, tr("Remote storage"), error);
        }
        return;
    }
    auto* dialog = new RemoteUploadDialog(localFile, kind, settings, credentials, parent);
    dialog->begin();
}

}
