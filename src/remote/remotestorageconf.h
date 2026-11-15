// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "remote/remotestorage.h"

#include <QWidget>

class QCheckBox;
class QComboBox;
class QFormLayout;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;

namespace RemoteStorage {

// The "Remote Storage" configuration tab: one shared connection plus a separate
// image and video destination and conversion profile.
class RemoteStorageConf : public QWidget
{
    Q_OBJECT
public:
    explicit RemoteStorageConf(QWidget* parent = nullptr);

    Settings settings() const;
    void setSettings(const Settings& settings);

public slots:
    void updateComponents();

private:
    void save();
    void updateVisibility();
    void updateImageFormatControls();
    void updateVideoCodecOptions();
    void checkSettings();
    void browseForFile(QLineEdit* edit, const QString& title);

    QCheckBox* m_enabled;
    QComboBox* m_protocol;
    QLineEdit* m_host;
    QSpinBox* m_port;
    QLineEdit* m_user;
    QLineEdit* m_privateKey;
    QLineEdit* m_knownHosts;
    QLineEdit* m_remoteDirectory;
    QCheckBox* m_acceptInsecureFtp;
    QComboBox* m_secretSource;
    QLabel* m_secretHint;
    QLabel* m_guaranteeLabel;

    QLineEdit* m_s3Endpoint;
    QLineEdit* m_s3Region;
    QLineEdit* m_s3Bucket;
    QLineEdit* m_s3KeyPrefix;
    QLineEdit* m_s3AccessKey;
    QLabel* m_s3Hint;

    QComboBox* m_imageMode;
    QLineEdit* m_imageDirectory;
    QComboBox* m_imageFormat;
    QSpinBox* m_imageQuality;
    QSpinBox* m_imageCompression;
    QLabel* m_imageHint;

    QComboBox* m_videoMode;
    QLineEdit* m_videoDirectory;
    QComboBox* m_videoContainer;
    QComboBox* m_videoCodec;
    QComboBox* m_videoRateControl;
    QSpinBox* m_videoQuality;
    QSpinBox* m_videoBitrate;
    QComboBox* m_videoAudioMode;
    QComboBox* m_videoAudioCodec;
    QSpinBox* m_videoAudioBitrate;
    QLabel* m_videoHint;

    QLabel* m_status;
    QPushButton* m_check;

    QFormLayout* m_connectionForm;
    QFormLayout* m_s3Form;
    QFormLayout* m_imageForm;
    QFormLayout* m_videoForm;
    bool m_loading = false;
};

}
