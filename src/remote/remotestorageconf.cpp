// SPDX-License-Identifier: GPL-3.0-or-later
#include "remote/remotestorageconf.h"
#include "remote/mediaexporter.h"
#include "remote/remotestoragestore.h"
#include "remote/remoteuploader.h"
#include "remote/remotestoragekeys.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QVBoxLayout>

#include <algorithm>

namespace RemoteStorage {
namespace {

QWidget* fileRow(QLineEdit*& edit, QWidget* parent)
{
    auto* row = new QWidget(parent);
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    edit = new QLineEdit(row);
    auto* browse = new QPushButton(QObject::tr("Browse…"), row);
    browse->setObjectName(QStringLiteral("browse"));
    layout->addWidget(edit);
    layout->addWidget(browse);
    return row;
}

QGroupBox* section(const QString& title, QWidget* parent, QFormLayout** form)
{
    auto* box = new QGroupBox(title, parent);
    auto* layout = new QFormLayout(box);
    if (form != nullptr) {
        *form = layout;
    }
    return box;
}

void addProtocolItem(QComboBox* combo, Protocol protocol, const QString& label)
{
    combo->addItem(label, static_cast<int>(protocol));
}

} // namespace

RemoteStorageConf::RemoteStorageConf(QWidget* parent)
  : QWidget(parent)
{
    auto* outer = new QVBoxLayout(this);
    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    outer->addWidget(scroll);
    auto* content = new QWidget(scroll);
    scroll->setWidget(content);
    auto* layout = new QVBoxLayout(content);

    m_enabled = new QCheckBox(tr("Upload saved screenshots and recordings to remote storage"), content);
    layout->addWidget(m_enabled);

    auto* connectionBox = section(tr("Connection"), content, &m_connectionForm);
    layout->addWidget(connectionBox);
    m_protocol = new QComboBox(connectionBox);
    addProtocolItem(m_protocol, Protocol::SFTP, tr("SFTP (SSH transfer subsystem)"));
    addProtocolItem(m_protocol, Protocol::SSH, tr("SSH (streamed over ssh)"));
    addProtocolItem(m_protocol, Protocol::FTP, tr("FTP (unencrypted)"));
    addProtocolItem(m_protocol, Protocol::S3, tr("S3 compatible storage"));
    m_connectionForm->addRow(tr("Protocol:"), m_protocol);
    m_host = new QLineEdit(connectionBox);
    m_connectionForm->addRow(tr("Server host:"), m_host);
    m_port = new QSpinBox(connectionBox);
    m_port->setRange(0, 65535);
    m_port->setSpecialValueText(tr("Default"));
    m_connectionForm->addRow(tr("Port:"), m_port);
    m_user = new QLineEdit(connectionBox);
    m_connectionForm->addRow(tr("Username:"), m_user);
    auto* keyRow = fileRow(m_privateKey, connectionBox);
    m_connectionForm->addRow(tr("SSH private key:"), keyRow);
    auto* knownHostsRow = fileRow(m_knownHosts, connectionBox);
    m_connectionForm->addRow(tr("SSH known_hosts:"), knownHostsRow);
    m_remoteDirectory = new QLineEdit(connectionBox);
    m_remoteDirectory->setPlaceholderText(QStringLiteral("/screenshots"));
    m_connectionForm->addRow(tr("Remote directory:"), m_remoteDirectory);
    m_acceptInsecureFtp = new QCheckBox(tr("I accept that FTP sends the password and the file unencrypted"),
                                        connectionBox);
    m_connectionForm->addRow(m_acceptInsecureFtp);
    m_secretSource = new QComboBox(connectionBox);
    m_secretSource->addItem(tr("Ask me at upload time"), static_cast<int>(SecretSource::Prompt));
    m_secretSource->addItem(tr("Read from the environment"), static_cast<int>(SecretSource::Environment));
    m_connectionForm->addRow(tr("Secret source:"), m_secretSource);
    m_guaranteeLabel = new QLabel(connectionBox);
    m_guaranteeLabel->setWordWrap(true);
    m_connectionForm->addRow(m_guaranteeLabel);
    m_secretHint = new QLabel(connectionBox);
    m_secretHint->setWordWrap(true);
    m_connectionForm->addRow(m_secretHint);

    auto* s3Box = section(tr("S3 compatible storage"), content, &m_s3Form);
    layout->addWidget(s3Box);
    m_s3Endpoint = new QLineEdit(s3Box);
    m_s3Endpoint->setPlaceholderText(QStringLiteral("https://s3.eu-west-1.amazonaws.com"));
    m_s3Form->addRow(tr("Endpoint:"), m_s3Endpoint);
    m_s3Region = new QLineEdit(s3Box);
    m_s3Form->addRow(tr("Region:"), m_s3Region);
    m_s3Bucket = new QLineEdit(s3Box);
    m_s3Form->addRow(tr("Bucket:"), m_s3Bucket);
    m_s3KeyPrefix = new QLineEdit(s3Box);
    m_s3KeyPrefix->setPlaceholderText(QStringLiteral("flameshot"));
    m_s3Form->addRow(tr("Object key prefix:"), m_s3KeyPrefix);
    m_s3AccessKey = new QLineEdit(s3Box);
    m_s3Form->addRow(tr("Access key ID:"), m_s3AccessKey);
    m_s3Hint = new QLabel(s3Box);
    m_s3Hint->setWordWrap(true);
    m_s3Form->addRow(m_s3Hint);

    auto* imageBox = section(tr("Screenshots"), content, &m_imageForm);
    layout->addWidget(imageBox);
    m_imageMode = new QComboBox(imageBox);
    m_imageMode->addItem(tr("Use original file"), static_cast<int>(Conversion::Original));
    m_imageMode->addItem(tr("Convert before upload"), static_cast<int>(Conversion::Convert));
    m_imageForm->addRow(tr("Upload:"), m_imageMode);
    m_imageDirectory = new QLineEdit(imageBox);
    m_imageDirectory->setPlaceholderText(tr("Empty: use the connection directory"));
    m_imageForm->addRow(tr("Remote directory or prefix:"), m_imageDirectory);
    m_imageFormat = new QComboBox(imageBox);
    m_imageForm->addRow(tr("Image format:"), m_imageFormat);
    m_imageQuality = new QSpinBox(imageBox);
    m_imageQuality->setRange(0, 100);
    m_imageForm->addRow(tr("Quality:"), m_imageQuality);
    m_imageCompression = new QSpinBox(imageBox);
    m_imageCompression->setRange(-1, 9);
    m_imageCompression->setSpecialValueText(tr("Format default"));
    m_imageForm->addRow(tr("Compression level:"), m_imageCompression);
    m_imageHint = new QLabel(imageBox);
    m_imageHint->setWordWrap(true);
    m_imageForm->addRow(m_imageHint);

    auto* videoBox = section(tr("Recordings"), content, &m_videoForm);
    layout->addWidget(videoBox);
    m_videoMode = new QComboBox(videoBox);
    m_videoMode->addItem(tr("Use original file"), static_cast<int>(Conversion::Original));
    m_videoMode->addItem(tr("Transcode before upload"), static_cast<int>(Conversion::Convert));
    m_videoForm->addRow(tr("Upload:"), m_videoMode);
    m_videoDirectory = new QLineEdit(videoBox);
    m_videoDirectory->setPlaceholderText(tr("Empty: use the connection directory"));
    m_videoForm->addRow(tr("Remote directory or prefix:"), m_videoDirectory);
    m_videoContainer = new QComboBox(videoBox);
    for (const QString& container : videoContainers()) {
        m_videoContainer->addItem(container);
    }
    m_videoForm->addRow(tr("Container:"), m_videoContainer);
    m_videoCodec = new QComboBox(videoBox);
    m_videoForm->addRow(tr("Encoder:"), m_videoCodec);
    m_videoRateControl = new QComboBox(videoBox);
    m_videoRateControl->addItem(tr("Quality (CRF)"), QStringLiteral("crf"));
    m_videoRateControl->addItem(tr("Average bitrate"), QStringLiteral("abr"));
    m_videoForm->addRow(tr("Rate control:"), m_videoRateControl);
    m_videoQuality = new QSpinBox(videoBox);
    // The range follows the selected encoder: x264 accepts 0-51, libvpx 0-63.
    // updateVideoCodecOptions() applies it and clamps the stored value.
    m_videoQuality->setRange(0, 63);
    m_videoForm->addRow(tr("Quality (CRF):"), m_videoQuality);
    m_videoBitrate = new QSpinBox(videoBox);
    m_videoBitrate->setRange(100, 100000);
    m_videoBitrate->setSuffix(tr(" kbit/s"));
    m_videoForm->addRow(tr("Average bitrate:"), m_videoBitrate);
    m_videoAudioMode = new QComboBox(videoBox);
    m_videoAudioMode->addItem(tr("Copy the original audio"), QStringLiteral("copy"));
    m_videoAudioMode->addItem(tr("Re-encode the audio"), QStringLiteral("encode"));
    m_videoAudioMode->addItem(tr("Drop the audio"), QStringLiteral("none"));
    m_videoForm->addRow(tr("Audio:"), m_videoAudioMode);
    m_videoAudioCodec = new QComboBox(videoBox);
    m_videoForm->addRow(tr("Audio encoder:"), m_videoAudioCodec);
    m_videoAudioBitrate = new QSpinBox(videoBox);
    m_videoAudioBitrate->setRange(32, 512);
    m_videoAudioBitrate->setSuffix(tr(" kbit/s"));
    m_videoForm->addRow(tr("Audio bitrate:"), m_videoAudioBitrate);
    m_videoHint = new QLabel(videoBox);
    m_videoHint->setWordWrap(true);
    m_videoForm->addRow(m_videoHint);

    auto* footer = new QHBoxLayout;
    m_check = new QPushButton(tr("Check settings"), content);
    m_status = new QLabel(content);
    m_status->setWordWrap(true);
    footer->addWidget(m_check);
    footer->addWidget(m_status, 1);
    layout->addLayout(footer);
    layout->addStretch(1);

    connect(m_check, &QPushButton::clicked, this, [this]() { checkSettings(); });
    connect(m_enabled, &QCheckBox::toggled, this, [this]() { save(); });
    connect(m_protocol, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this]() { save(); updateVisibility(); });
    connect(m_port, &QSpinBox::valueChanged, this, [this]() { save(); });
    for (QLineEdit* edit : { m_host, m_user, m_privateKey, m_knownHosts, m_remoteDirectory,
                             m_s3Endpoint, m_s3Region, m_s3Bucket, m_s3KeyPrefix,
                             m_s3AccessKey, m_imageDirectory, m_videoDirectory }) {
        connect(edit, &QLineEdit::editingFinished, this, [this]() { save(); });
    }
    connect(m_acceptInsecureFtp, &QCheckBox::toggled, this, [this]() { save(); });
    connect(m_secretSource, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this]() { save(); updateVisibility(); });
    connect(m_imageMode, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this]() { save(); updateVisibility(); });
    connect(m_imageFormat, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this]() { save(); updateImageFormatControls(); });
    connect(m_imageQuality, &QSpinBox::valueChanged, this, [this]() { save(); });
    connect(m_imageCompression, &QSpinBox::valueChanged, this, [this]() { save(); });
    connect(m_videoMode, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this]() { save(); updateVisibility(); });
    connect(m_videoContainer, &QComboBox::currentTextChanged, this, [this]() {
        updateVideoCodecOptions();
        save();
    });
    for (QComboBox* combo : { m_videoCodec, m_videoRateControl, m_videoAudioMode, m_videoAudioCodec }) {
        connect(combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
                [this]() { save(); updateVisibility(); });
    }
    for (QSpinBox* spin : { m_videoQuality, m_videoBitrate, m_videoAudioBitrate }) {
        connect(spin, &QSpinBox::valueChanged, this, [this]() { save(); });
    }

    for (QLineEdit* edit : { m_privateKey, m_knownHosts }) {
        auto* row = qobject_cast<QWidget*>(edit->parentWidget());
        auto* browse = row != nullptr ? row->findChild<QPushButton*>(QStringLiteral("browse"))
                                      : nullptr;
        if (browse != nullptr) {
            connect(browse, &QPushButton::clicked, this, [this, edit]() {
                browseForFile(edit, tr("Choose a file"));
            });
        }
    }

    updateComponents();
}

Settings RemoteStorageConf::settings() const
{
    Settings settings;
    settings.enabled = m_enabled->isChecked();
    Connection& connection = settings.connection;
    connection.protocol = static_cast<Protocol>(m_protocol->currentData().toInt());
    connection.host = m_host->text().trimmed();
    connection.port = static_cast<quint16>(m_port->value());
    connection.username = m_user->text().trimmed();
    connection.privateKeyPath = m_privateKey->text().trimmed();
    connection.knownHostsPath = m_knownHosts->text().trimmed();
    connection.remoteDirectory = m_remoteDirectory->text().trimmed();
    connection.acceptInsecureFtp = m_acceptInsecureFtp->isChecked();
    connection.secretSource = static_cast<SecretSource>(m_secretSource->currentData().toInt());
    connection.endpoint = m_s3Endpoint->text().trimmed();
    connection.region = m_s3Region->text().trimmed();
    connection.bucket = m_s3Bucket->text().trimmed();
    connection.keyPrefix = m_s3KeyPrefix->text().trimmed();
    connection.accessKeyId = m_s3AccessKey->text().trimmed();

    settings.image.conversion = static_cast<Conversion>(m_imageMode->currentData().toInt());
    settings.image.directory = m_imageDirectory->text().trimmed();
    settings.image.format = m_imageFormat->currentData().toString();
    settings.image.quality = m_imageQuality->value();
    settings.image.compression = m_imageCompression->value();

    settings.video.conversion = static_cast<Conversion>(m_videoMode->currentData().toInt());
    settings.video.directory = m_videoDirectory->text().trimmed();
    settings.video.container = m_videoContainer->currentText();
    settings.video.codec = m_videoCodec->currentData().toString();
    settings.video.rateControl = m_videoRateControl->currentData().toString();
    settings.video.quality = m_videoQuality->value();
    settings.video.bitrateKbps = m_videoBitrate->value();
    settings.video.audioMode = m_videoAudioMode->currentData().toString();
    settings.video.audioCodec = m_videoAudioCodec->currentData().toString();
    settings.video.audioBitrateKbps = m_videoAudioBitrate->value();
    return settings;
}

void RemoteStorageConf::setSettings(const Settings& settings)
{
    const QSignalBlocker blocker(this);
    m_loading = true;
    m_enabled->setChecked(settings.enabled);
    m_protocol->setCurrentIndex(
      std::max(0, m_protocol->findData(static_cast<int>(settings.connection.protocol))));
    m_host->setText(settings.connection.host);
    m_port->setValue(settings.connection.port);
    m_user->setText(settings.connection.username);
    m_privateKey->setText(settings.connection.privateKeyPath);
    m_knownHosts->setText(settings.connection.knownHostsPath);
    m_remoteDirectory->setText(settings.connection.remoteDirectory);
    m_acceptInsecureFtp->setChecked(settings.connection.acceptInsecureFtp);
    m_secretSource->setCurrentIndex(std::max(
      0, m_secretSource->findData(static_cast<int>(settings.connection.secretSource))));
    m_s3Endpoint->setText(settings.connection.endpoint);
    m_s3Region->setText(settings.connection.region);
    m_s3Bucket->setText(settings.connection.bucket);
    m_s3KeyPrefix->setText(settings.connection.keyPrefix);
    m_s3AccessKey->setText(settings.connection.accessKeyId);

    m_imageMode->setCurrentIndex(
      std::max(0, m_imageMode->findData(static_cast<int>(settings.image.conversion))));
    m_imageDirectory->setText(settings.image.directory);
    const int formatIndex = m_imageFormat->findData(settings.image.format);
    m_imageFormat->setCurrentIndex(std::max(0, formatIndex));
    m_imageQuality->setValue(settings.image.quality);
    m_imageCompression->setValue(settings.image.compression);

    m_videoMode->setCurrentIndex(
      std::max(0, m_videoMode->findData(static_cast<int>(settings.video.conversion))));
    m_videoDirectory->setText(settings.video.directory);
    const int containerIndex = m_videoContainer->findText(settings.video.container);
    m_videoContainer->setCurrentIndex(std::max(0, containerIndex));
    updateVideoCodecOptions();
    const int codecIndex = m_videoCodec->findData(settings.video.codec);
    m_videoCodec->setCurrentIndex(std::max(0, codecIndex));
    const int rateIndex = m_videoRateControl->findData(settings.video.rateControl);
    m_videoRateControl->setCurrentIndex(std::max(0, rateIndex));
    m_videoQuality->setValue(settings.video.quality);
    m_videoBitrate->setValue(settings.video.bitrateKbps);
    const int audioModeIndex = m_videoAudioMode->findData(settings.video.audioMode);
    m_videoAudioMode->setCurrentIndex(std::max(0, audioModeIndex));
    updateVideoCodecOptions();
    const int audioCodecIndex = m_videoAudioCodec->findData(settings.video.audioCodec);
    m_videoAudioCodec->setCurrentIndex(std::max(0, audioCodecIndex));
    m_videoAudioBitrate->setValue(settings.video.audioBitrateKbps);

    m_loading = false;
    updateImageFormatControls();
    updateVisibility();
}

void RemoteStorageConf::updateComponents()
{
    m_loading = true;
    for (const ImageFormatInfo& info : imageFormats()) {
        if (m_imageFormat->findData(info.name) >= 0) {
            continue;
        }
        m_imageFormat->addItem(info.name.toUpper() + QStringLiteral(" (.") + info.suffix +
                                 QLatin1Char(')'),
                               info.name);
    }
    m_loading = false;
    setSettings(configuredSettings());
}

void RemoteStorageConf::updateImageFormatControls()
{
    const QString token = m_imageFormat->currentData().toString();
    const bool converting =
      static_cast<Conversion>(m_imageMode->currentData().toInt()) == Conversion::Convert;
    const ImageFormatInfo info = imageFormat(token);
    m_imageQuality->setEnabled(converting && info.supportsQuality);
    m_imageCompression->setEnabled(converting && info.supportsCompression);
    if (!converting) {
        m_imageHint->setText(tr("The saved screenshot is uploaded unchanged."));
    } else if (token.isEmpty()) {
        m_imageHint->setText(tr("Choose an image format; the file is re-encoded with the "
                                "Qt image writer for that format."));
    } else {
        QString hint = tr("Re-encoded with the %1 writer.").arg(token.toUpper());
        if (!info.supportsQuality) {
            hint += QLatin1Char(' ') + tr("This format has no quality setting.");
        }
        if (!info.supportsCompression) {
            hint += QLatin1Char(' ') + tr("This format has no compression level.");
        }
        m_imageHint->setText(hint);
    }
}

void RemoteStorageConf::updateVideoCodecOptions()
{
    const QString container = m_videoContainer->currentText();
    const QString previousCodec = m_videoCodec->currentData().toString();
    const QString previousAudio = m_videoAudioCodec->currentData().toString();
    {
        const QSignalBlocker blocker(m_videoCodec);
        m_videoCodec->clear();
        for (const QString& codec : videoCodecs(container)) {
            m_videoCodec->addItem(codec, codec);
        }
        const int index = m_videoCodec->findData(previousCodec);
        m_videoCodec->setCurrentIndex(index >= 0 ? index : 0);
    }
    {
        const QSignalBlocker blocker(m_videoAudioCodec);
        m_videoAudioCodec->clear();
        for (const QString& codec : videoAudioCodecs(container)) {
            m_videoAudioCodec->addItem(codec, codec);
        }
        const int index = m_videoAudioCodec->findData(previousAudio);
        m_videoAudioCodec->setCurrentIndex(index >= 0 ? index : 0);
    }

    // Keep the quality control within what the selected encoder accepts, so a
    // value the encoder rejects can never be chosen or kept.
    const QualityRange range =
      videoQualityRange(container, m_videoCodec->currentData().toString());
    {
        const QSignalBlocker blocker(m_videoQuality);
        m_videoQuality->setRange(range.minimum, range.maximum);
        m_videoQuality->setValue(std::clamp(m_videoQuality->value(), range.minimum,
                                            range.maximum));
    }
    m_videoQuality->setToolTip(tr("Accepted range for this encoder: %1-%2.")
                                 .arg(range.minimum)
                                 .arg(range.maximum));
}

void RemoteStorageConf::updateVisibility()
{
    const auto protocol = static_cast<Protocol>(m_protocol->currentData().toInt());
    const bool objectStorage = protocol == Protocol::S3;
    const bool fileTransfer = !objectStorage;

    m_connectionForm->setRowVisible(m_host, fileTransfer);
    m_connectionForm->setRowVisible(m_port, fileTransfer);
    m_connectionForm->setRowVisible(m_user, fileTransfer);
    m_connectionForm->setRowVisible(m_remoteDirectory, fileTransfer);
    const bool sshProtocol = protocol == Protocol::SFTP || protocol == Protocol::SSH;
    for (QLineEdit* edit : { m_privateKey, m_knownHosts }) {
        QWidget* row = edit->parentWidget();
        if (row != nullptr) {
            m_connectionForm->setRowVisible(row, sshProtocol);
        }
    }
    m_connectionForm->setRowVisible(m_acceptInsecureFtp, protocol == Protocol::FTP);
    m_connectionForm->setRowVisible(
      m_s3Endpoint, objectStorage);

    for (QLineEdit* edit : { m_s3Endpoint, m_s3Region, m_s3Bucket, m_s3KeyPrefix, m_s3AccessKey }) {
        m_s3Form->setRowVisible(edit, objectStorage);
    }
    m_s3Form->setRowVisible(m_s3Hint, objectStorage);

    const bool needsSecret = protocol == Protocol::S3 || protocol == Protocol::FTP;
    m_connectionForm->setRowVisible(m_secretSource, needsSecret);
    m_connectionForm->setRowVisible(m_secretHint, needsSecret);
    if (protocol == Protocol::FTP) {
        m_secretHint->setText(tr("FTP has no host verification: the password and the file "
                                 "travel unencrypted. The password is never written to the "
                                 "configuration file."));
    } else if (objectStorage) {
        m_secretHint->setText(tr("Signed with AWS Signature Version 4. The secret access key is "
                                 "never written to the configuration file: set %1 (and %2) or "
                                 "answer the prompt.")
                                .arg(QStringLiteral(REMOTE_STORAGE_ENV_S3_SECRET_KEY),
                                     QStringLiteral(REMOTE_STORAGE_ENV_S3_SESSION_TOKEN)));
    } else {
        m_secretHint->clear();
    }
    m_guaranteeLabel->setText(noReplaceGuarantee(protocol));
    m_s3Hint->setText(tr("Requests are signed with AWS Signature Version 4. The secret access "
                         "key is requested at upload time and is never stored. Plain http is "
                         "accepted only for a loopback endpoint."));

    const bool imageConverting =
      static_cast<Conversion>(m_imageMode->currentData().toInt()) == Conversion::Convert;
    m_imageForm->setRowVisible(m_imageDirectory, true);
    m_imageForm->setRowVisible(m_imageFormat, imageConverting);
    m_imageForm->setRowVisible(m_imageQuality, imageConverting);
    m_imageForm->setRowVisible(m_imageCompression, imageConverting);

    const bool videoConverting =
      static_cast<Conversion>(m_videoMode->currentData().toInt()) == Conversion::Convert;
    for (QWidget* widget : { static_cast<QWidget*>(m_videoContainer),
                             static_cast<QWidget*>(m_videoCodec),
                             static_cast<QWidget*>(m_videoRateControl),
                             static_cast<QWidget*>(m_videoQuality),
                             static_cast<QWidget*>(m_videoBitrate),
                             static_cast<QWidget*>(m_videoAudioMode),
                             static_cast<QWidget*>(m_videoAudioCodec),
                             static_cast<QWidget*>(m_videoAudioBitrate) }) {
        m_videoForm->setRowVisible(widget, videoConverting);
    }
    m_videoQuality->setEnabled(
      videoConverting && m_videoRateControl->currentData().toString() == QLatin1String("crf"));
    m_videoBitrate->setEnabled(
      videoConverting && m_videoRateControl->currentData().toString() == QLatin1String("abr"));
    const bool audioEncoding = videoConverting &&
                               m_videoAudioMode->currentData().toString() == QLatin1String("encode");
    m_videoAudioCodec->setEnabled(audioEncoding);
    m_videoAudioBitrate->setEnabled(audioEncoding);
    if (videoConverting) {
        m_videoHint->setText(tr("FFmpeg converts the recording into a temporary file; the saved "
                                "recording itself is never modified. The FFmpeg executable comes "
                                "from Configuration > Video."));
    } else {
        m_videoHint->setText(tr("The saved recording is uploaded unchanged."));
    }
    m_check->setEnabled(m_enabled->isChecked());
}

void RemoteStorageConf::save()
{
    if (m_loading) {
        return;
    }
    saveConfiguredSettings(settings());
}

void RemoteStorageConf::checkSettings()
{
    const Settings current = settings();
    QString message = validate(current, MediaKind::Image);
    if (message.isEmpty()) {
        message = validate(current, MediaKind::Video);
    }
    if (message.isEmpty()) {
        m_status->setText(tr("The settings look usable. Nothing was uploaded."));
    } else {
        m_status->setText(message);
    }
}

void RemoteStorageConf::browseForFile(QLineEdit* edit, const QString& title)
{
    const QString path = QFileDialog::getOpenFileName(this, title, edit->text());
    if (!path.isEmpty()) {
        edit->setText(path);
        save();
    }
}

}
