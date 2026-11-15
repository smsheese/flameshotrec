// SPDX-License-Identifier: GPL-3.0-or-later
#include "videoconf.h"
#include "utils/confighandler.h"
#include "recording/annotationpolicy.h"
#include "recording/annotationpolicy_confighandler.h"
#include "recording/audiocapture.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFileDialog>
#include <QFont>
#include <QGridLayout>
#include <QGroupBox>
#include <QProcess>
#include <QTimer>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>
#include <QSignalBlocker>
#include <QStandardItemModel>

namespace {
// Rebuilds a device selector, keeping an explicitly configured source visible
// even when the sound server currently does not report it.
void fillSourceCombo(QComboBox* combo,
                     const QList<Recording::AudioSourceInfo>& sources,
                     bool monitors,
                     const QString& selected,
                     const QString& defaultLabel)
{
    const QSignalBlocker blocker(combo);
    combo->clear();
    combo->addItem(defaultLabel, QString());
    bool found = selected.trimmed().isEmpty();
    for (const auto& source : sources) {
        if (source.monitor != monitors) continue;
        combo->addItem(source.description, source.name);
        combo->setItemData(combo->count() - 1, source.name, Qt::ToolTipRole);
        if (source.name == selected) {
            combo->setCurrentIndex(combo->count() - 1);
            found = true;
        }
    }
    if (!found) {
        combo->addItem(QObject::tr("%1 (not detected)").arg(selected), selected);
        combo->setCurrentIndex(combo->count() - 1);
    }
    if (combo->currentIndex() < 0) combo->setCurrentIndex(0);
}
} // namespace

VideoConf::VideoConf(QWidget* parent) : QWidget(parent)
{
    auto* root = new QVBoxLayout(this);
    auto* form = new QFormLayout;
    root->addLayout(form);
    auto* pathRow = new QWidget(this);
    auto* pathLayout = new QHBoxLayout(pathRow);
    pathLayout->setContentsMargins(0, 0, 0, 0);
    m_ffmpegPath = new QLineEdit(pathRow);
    m_ffmpegPath->setObjectName(QStringLiteral("ffmpegPath"));
    m_ffmpegPath->setPlaceholderText(tr("Auto-detect from PATH and common locations"));
    auto* browse = new QPushButton(tr("Browse…"), pathRow);
    pathLayout->addWidget(m_ffmpegPath);
    pathLayout->addWidget(browse);
    form->addRow(tr("FFmpeg executable:"), pathRow);
    connect(browse, &QPushButton::clicked, this, [this]() {
        const auto path = QFileDialog::getOpenFileName(this, tr("Select FFmpeg executable"), m_ffmpegPath->text());
        if (!path.isEmpty()) m_ffmpegPath->setText(path);
    });
    connect(m_ffmpegPath, &QLineEdit::editingFinished, this, [this]() {
        ConfigHandler().setRecordingFfmpegPath(m_ffmpegPath->text().trimmed());
    });
    auto* videoPathRow = new QWidget(this);
    auto* videoPathLayout = new QHBoxLayout(videoPathRow);
    videoPathLayout->setContentsMargins(0, 0, 0, 0);
    m_videoPath = new QLineEdit(videoPathRow);
    m_videoPath->setObjectName(QStringLiteral("videoPath"));
    auto* videoPathBrowse = new QPushButton(tr("Browse…"), videoPathRow);
    videoPathLayout->addWidget(m_videoPath);
    videoPathLayout->addWidget(videoPathBrowse);
    form->addRow(tr("Video save folder:"), videoPathRow);
    connect(videoPathBrowse, &QPushButton::clicked, this, [this]() {
        const QString path = QFileDialog::getExistingDirectory(
          this, tr("Choose video save folder"), m_videoPath->text());
        if (!path.isEmpty()) {
            m_videoPath->setText(path);
            ConfigHandler().setRecordingVideoPath(path);
        }
    });
    connect(m_videoPath, &QLineEdit::editingFinished, this, [this]() {
        const QString path = m_videoPath->text().trimmed();
        if (!path.isEmpty()) ConfigHandler().setRecordingVideoPath(path);
    });
    m_filenamePattern = new QLineEdit(this);
    m_filenamePattern->setObjectName(QStringLiteral("videoFilenamePattern"));
    m_filenamePattern->setToolTip(tr("strftime pattern used for video filenames"));
    form->addRow(tr("Video filename pattern:"), m_filenamePattern);
    connect(m_filenamePattern, &QLineEdit::editingFinished, this, [this]() {
        ConfigHandler().setRecordingFilenamePattern(m_filenamePattern->text().trimmed());
    });
    m_frameRate = new QSpinBox(this);
    m_frameRate->setObjectName(QStringLiteral("frameRate"));
    m_frameRate->setRange(1, 120);
    form->addRow(tr("Frame rate:"), m_frameRate);
    connect(m_frameRate, &QSpinBox::valueChanged, this, [](int v) { ConfigHandler().setRecordingFrameRate(v); });
    m_container = new QComboBox(this);
    m_container->setObjectName(QStringLiteral("container"));
    m_container->addItems({"mp4", "mkv", "webm"});
    form->addRow(tr("Container:"), m_container);
    connect(m_container, &QComboBox::currentTextChanged, this, [](const QString& v) { ConfigHandler().setRecordingContainer(v); });
    m_codec = new QComboBox(this);
    m_codec->setObjectName(QStringLiteral("codec"));
    m_codec->addItem(tr("H.264 (libx264)"), "libx264");
    m_codec->addItem(tr("VP9 (libvpx-vp9)"), "libvpx-vp9");
    form->addRow(tr("Encoder:"), m_codec);
    connect(m_codec, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int i) { ConfigHandler().setRecordingCodec(m_codec->itemData(i).toString()); });
    m_rateControl = new QComboBox(this);
    m_rateControl->setObjectName(QStringLiteral("rateControl"));
    m_rateControl->addItem(tr("Quality (CRF)"), "crf");
    m_rateControl->addItem(tr("Average bitrate (ABR)"), "abr");
    form->addRow(tr("Rate control:"), m_rateControl);
    connect(m_rateControl, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int i) { ConfigHandler().setRecordingRateControl(m_rateControl->itemData(i).toString()); });
    m_bitrate = new QSpinBox(this);
    m_bitrate->setObjectName(QStringLiteral("bitrate"));
    m_bitrate->setRange(100, 100000);
    m_bitrate->setSuffix(tr(" kbit/s"));
    form->addRow(tr("Average bitrate:"), m_bitrate);
    connect(m_bitrate, &QSpinBox::valueChanged, this, [](int v) { ConfigHandler().setRecordingBitrateKbps(v); });
    m_quality = new QSpinBox(this);
    m_quality->setObjectName(QStringLiteral("quality"));
    m_quality->setRange(0, 63);
    form->addRow(tr("Quality (CRF):"), m_quality);
    connect(m_quality, &QSpinBox::valueChanged, this, [](int v) { ConfigHandler().setRecordingQuality(v); });
    m_holdDuration = new QSpinBox(this);
    m_holdDuration->setObjectName(QStringLiteral("globalHoldDuration"));
    m_holdDuration->setRange(0, 30000);
    m_holdDuration->setSuffix(tr(" ms"));
    form->addRow(tr("Marking hold time:"), m_holdDuration);
    connect(m_holdDuration, &QSpinBox::valueChanged, this, [this](int v) {
        ConfigHandler().setRecordingHoldDuration(v);
        // The per-tool "Default" entries name the global value they inherit.
        m_globalHoldMs = v;
        updateInheritTexts();
    });
    m_fadeDuration = new QSpinBox(this);
    m_fadeDuration->setObjectName(QStringLiteral("globalFadeDuration"));
    m_fadeDuration->setRange(0, 10000);
    m_fadeDuration->setSuffix(tr(" ms"));
    form->addRow(tr("Marking fade time:"), m_fadeDuration);
    connect(m_fadeDuration, &QSpinBox::valueChanged, this, [this](int v) {
        ConfigHandler().setRecordingFadeDuration(v);
        m_globalFadeMs = v;
        updateInheritTexts();
    });
    m_audioSystem = new QCheckBox(tr("Record system audio (speakers)"), this);
    m_audioSystem->setObjectName(QStringLiteral("audioSystem"));
    m_audioSystem->setToolTip(
      tr("Captures the monitor of the current default output device."));
    form->addRow(tr("System audio:"), m_audioSystem);
    connect(m_audioSystem, &QCheckBox::toggled, this, [this](bool checked) {
        ConfigHandler().setRecordingAudioSystemEnabled(checked);
        updateAudioControls();
    });
    m_audioSystemSource = new QComboBox(this);
    form->addRow(tr("Audio output source:"), m_audioSystemSource);
    connect(m_audioSystemSource,
            QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int index) {
        if (index < 0) return;
        ConfigHandler().setRecordingAudioSystemSource(
          m_audioSystemSource->itemData(index).toString());
    });
    m_audioMic = new QCheckBox(tr("Record microphone"), this);
    m_audioMic->setObjectName(QStringLiteral("audioMic"));
    form->addRow(tr("Microphone:"), m_audioMic);
    connect(m_audioMic, &QCheckBox::toggled, this, [this](bool checked) {
        ConfigHandler().setRecordingAudioMicEnabled(checked);
        updateAudioControls();
    });
    m_audioMicSource = new QComboBox(this);
    form->addRow(tr("Microphone source:"), m_audioMicSource);
    connect(m_audioMicSource,
            QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int index) {
        if (index < 0) return;
        ConfigHandler().setRecordingAudioMicSource(
          m_audioMicSource->itemData(index).toString());
    });
    m_audioQuality = new QComboBox(this);
    m_audioQuality->setObjectName(QStringLiteral("audioQuality"));
    for (const QString& key : Recording::Audio::qualityKeys())
        m_audioQuality->addItem(Recording::Audio::qualityDisplayName(key), key);
    form->addRow(tr("Audio quality:"), m_audioQuality);
    connect(m_audioQuality, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int index) {
        if (index < 0) return;
        ConfigHandler().setRecordingAudioQuality(
          m_audioQuality->itemData(index).toString());
    });
    m_audioBitrate = new QSpinBox(this);
    m_audioBitrate->setObjectName(QStringLiteral("audioBitrate"));
    m_audioBitrate->setRange(32, 512);
    m_audioBitrate->setSuffix(tr(" kbit/s"));
    form->addRow(tr("Audio bitrate:"), m_audioBitrate);
    connect(m_audioBitrate, &QSpinBox::valueChanged, this,
            [](int value) { ConfigHandler().setRecordingAudioBitrateKbps(value); });
    m_audioStatus = new QLabel(this);
    m_audioStatus->setWordWrap(true);
    root->addWidget(m_audioStatus);
    // Settings for the annotations drawn on the live recording overlay. Kept at
    // the bottom of the tab: the video, encoder and audio controls are what an
    // ordinary recording needs, the per-tool lifetimes are refinement.
    root->addWidget(createAnnotationToolsSection(this));
    root->addWidget(new QLabel(tr("Recording requires a supported FFmpeg build. Leave the path blank to auto-detect."), this));
    root->addStretch();
    connect(m_container, &QComboBox::currentTextChanged, this, [this]() { updateEncoderOptions(); });
    connect(m_codec, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this]() { updateEncoderOptions(); });
    connect(m_rateControl, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this]() { updateEncoderOptions(); });
    updateComponents();
}

void VideoConf::updateEncoderOptions()
{
    if (m_codec->currentIndex() < 0) m_codec->setCurrentIndex(0);
    const QString container = m_container->currentText();
    const bool allowH264 = container != QLatin1String("webm");
    const bool allowVp9 = container != QLatin1String("mp4");
    auto* model = qobject_cast<QStandardItemModel*>(m_codec->model());
    if (model) {
        model->item(0)->setEnabled(allowH264);
        model->item(1)->setEnabled(allowVp9);
    }
    if (!allowH264 && m_codec->currentIndex() == 0) m_codec->setCurrentIndex(1);
    if (!allowVp9 && m_codec->currentIndex() == 1) m_codec->setCurrentIndex(0);
    m_quality->setMaximum(m_codec->currentData().toString() == QLatin1String("libx264") ? 51 : 63);
    const bool qualityMode = m_rateControl->currentData().toString() == QLatin1String("crf");
    m_quality->setEnabled(qualityMode);
    m_bitrate->setEnabled(!qualityMode);
    ConfigHandler config;
    config.setRecordingCodec(m_codec->currentData().toString());
    config.setRecordingQuality(m_quality->value());
}

void VideoConf::populateAudioDevices()
{
    // The device list is queried at most once per burst of configuration
    // changes, and never on the GUI thread's blocking path (review F4).
    if (!m_audioRefreshTimer) {
        m_audioRefreshTimer = new QTimer(this);
        m_audioRefreshTimer->setSingleShot(true);
        m_audioRefreshTimer->setInterval(200);
        connect(m_audioRefreshTimer, &QTimer::timeout, this, [this]() {
            queryAudioDevices();
        });
    }
    m_audioRefreshTimer->start();
}

void VideoConf::queryAudioDevices()
{
    if (m_audioQuery) {
        // A query is already running: repeat it once it finished so the last
        // configuration write wins.
        m_audioRefreshPending = true;
        return;
    }
    const QString pactl = Recording::Audio::pactlExecutable();
    if (pactl.isEmpty()) {
        applyAudioSources({},
                          tr("The 'pactl' tool was not found. Audio device "
                             "discovery needs pulseaudio-utils."));
        return;
    }
    m_audioQuery = new QProcess(this);
    m_audioQuery->setProcessChannelMode(QProcess::SeparateChannels);
    auto* timeout = new QTimer(m_audioQuery);
    timeout->setSingleShot(true);
    timeout->setInterval(4000);
    connect(timeout, &QTimer::timeout, m_audioQuery, [this]() {
        if (m_audioQuery) m_audioQuery->kill();
    });
    connect(m_audioQuery,
            qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
            this,
            [this](int code, QProcess::ExitStatus status) {
        QProcess* query = m_audioQuery;
        m_audioQuery = nullptr;
        if (!query) return;
        const QString output =
          QString::fromLocal8Bit(query->readAllStandardOutput());
        const QString failure =
          QString::fromLocal8Bit(query->readAllStandardError()).trimmed();
        query->deleteLater();
        if (status != QProcess::NormalExit || code != 0) {
            applyAudioSources(
              {},
              tr("Could not list audio devices: %1")
                .arg(failure.isEmpty() ? tr("pactl failed.") : failure));
        } else {
            applyAudioSources(Recording::Audio::parseSourceList(output),
                              QString());
        }
        if (m_audioRefreshPending) {
            m_audioRefreshPending = false;
            queryAudioDevices();
        }
    });
    timeout->start();
    m_audioQuery->start(pactl, { QStringLiteral("list"),
                                 QStringLiteral("sources") });
}

void VideoConf::applyAudioSources(
  const QList<Recording::AudioSourceInfo>& sources,
  const QString& error)
{
    ConfigHandler config;
    fillSourceCombo(m_audioSystemSource, sources, true,
                    config.recordingAudioSystemSource(),
                    tr("Follow the default output"));
    fillSourceCombo(m_audioMicSource, sources, false,
                    config.recordingAudioMicSource(),
                    tr("Follow the default input"));
    if (sources.isEmpty()) {
        m_audioStatus->setText(
          error.isEmpty()
            ? tr("The sound server reported no audio devices. Recording saves "
                 "video without sound until a device is available.")
            : error);
    } else {
        m_audioStatus->setText(
          tr("Audio is captured from PulseAudio or PipeWire. The selected "
             "audio codec follows the container: AAC for MP4/MKV, Opus for "
             "WebM."));
    }
}

void VideoConf::updateAudioControls()
{
    const bool systemAudio = m_audioSystem->isChecked();
    const bool microphone = m_audioMic->isChecked();
    m_audioSystemSource->setEnabled(systemAudio);
    m_audioMicSource->setEnabled(microphone);
    m_audioQuality->setEnabled(systemAudio || microphone);
    m_audioBitrate->setEnabled(systemAudio || microphone);
}

void VideoConf::updateComponents()
{
    const QSignalBlocker b0(m_videoPath), b00(m_filenamePattern),
      b1(m_ffmpegPath), b2(m_frameRate), b3(m_holdDuration),
      b4(m_fadeDuration), b5(m_container), b6(m_codec), b7(m_rateControl),
      b8(m_bitrate), b9(m_quality), b10(m_audioSystem), b11(m_audioMic),
      b12(m_audioQuality), b13(m_audioBitrate);
    ConfigHandler c;
    m_ffmpegPath->setText(c.recordingFfmpegPath());
    m_videoPath->setText(c.recordingVideoPath());
    m_filenamePattern->setText(c.recordingFilenamePattern());
    m_frameRate->setValue(c.recordingFrameRate());
    m_holdDuration->setValue(c.recordingHoldDuration());
    m_fadeDuration->setValue(c.recordingFadeDuration());
    m_container->setCurrentText(c.recordingContainer());
    m_codec->setCurrentIndex(m_codec->findData(c.recordingCodec()));
    m_rateControl->setCurrentIndex(m_rateControl->findData(c.recordingRateControl()));
    m_bitrate->setValue(c.recordingBitrateKbps());
    m_quality->setValue(c.recordingQuality());
    m_audioSystem->setChecked(c.recordingAudioSystemEnabled());
    m_audioMic->setChecked(c.recordingAudioMicEnabled());
    const int qualityIndex =
      m_audioQuality->findData(c.recordingAudioQuality());
    m_audioQuality->setCurrentIndex(qualityIndex < 0 ? 0 : qualityIndex);
    m_audioBitrate->setValue(c.recordingAudioBitrateKbps());
    updateEncoderOptions();
    populateAudioDevices();
    updateAudioControls();
    updateAnnotationToolsSection();
}

// ---------------------------------------------------------------------------
// Annotation tools section
//
// The per-tool lifetimes are an addition to the recording-wide "Marking hold
// time" / "Marking fade time" above: every tool starts on "Default", which is
// exactly the global value, so a user who never opens this section keeps the
// behaviour and the configuration they already have.
// ---------------------------------------------------------------------------

VideoConf::ToolRow* VideoConf::toolRow(Recording::AnnotationTool tool)
{
    for (auto& row : m_toolRows) {
        if (row.tool == tool) {
            return &row;
        }
    }
    return nullptr;
}

QWidget* VideoConf::createAnnotationToolsSection(QWidget* parent)
{
    auto* container = new QWidget(parent);
    auto* containerLayout = new QVBoxLayout(container);
    containerLayout->setContentsMargins(0, 0, 0, 0);

    m_toolSettingsStatus = new QLabel(container);
    m_toolSettingsStatus->setObjectName(QStringLiteral("annotationToolsStatus"));
    m_toolSettingsStatus->setWordWrap(true);
    m_toolSettingsStatus->hide();
    containerLayout->addWidget(m_toolSettingsStatus);

    m_toolSettings = new QGroupBox(tr("Annotation tools"), container);
    m_toolSettings->setObjectName(QStringLiteral("annotationToolsGroup"));
    containerLayout->addWidget(m_toolSettings);

    auto* layout = new QVBoxLayout(m_toolSettings);
    auto* intro = new QLabel(
      tr("How long each annotation drawn while recording stays on screen. "
         "\"Default\" uses the marking hold and fade times above."),
      m_toolSettings);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    auto* grid = new QGridLayout;
    grid->setColumnStretch(0, 1);

    const auto headerLabel = [this](const QString& text) {
        auto* label = new QLabel(text, m_toolSettings);
        QFont font = label->font();
        font.setBold(true);
        label->setFont(font);
        return label;
    };
    grid->addWidget(headerLabel(tr("Tool")), 0, 0);
    grid->addWidget(headerLabel(tr("Hold")), 0, 1);
    grid->addWidget(headerLabel(tr("Fade")), 0, 2);
    grid->addWidget(headerLabel(tr("Size")), 0, 3);

    int rowIndex = 1;
    for (const auto tool : Recording::AnnotationPolicy::supportedTools()) {
        ToolRow row;
        row.tool = tool;
        const QString identifier = Recording::AnnotationPolicy::toolIdentifier(tool);

        auto* name = new QLabel(Recording::AnnotationPolicy::displayName(tool),
                                m_toolSettings);
        grid->addWidget(name, rowIndex, 0);

        row.hold = new QSpinBox(m_toolSettings);
        row.hold->setObjectName(QStringLiteral("toolHold_") + identifier);
        row.hold->setRange(Recording::kInherit, Recording::kMaxHoldMs);
        row.hold->setSuffix(tr(" ms"));
        row.hold->setToolTip(
          tr("Full strength time for this tool before it starts to fade."));
        grid->addWidget(row.hold, rowIndex, 1);

        row.fade = new QSpinBox(m_toolSettings);
        row.fade->setObjectName(QStringLiteral("toolFade_") + identifier);
        row.fade->setRange(Recording::kInherit, Recording::kMaxFadeMs);
        row.fade->setSuffix(tr(" ms"));
        row.fade->setToolTip(tr("Fade out time for this tool."));
        grid->addWidget(row.fade, rowIndex, 2);

        if (Recording::AnnotationPolicy::sizeKind(tool) !=
            Recording::AnnotationSizeKind::None) {
            row.size = new QSpinBox(m_toolSettings);
            row.size->setObjectName(QStringLiteral("toolSize_") + identifier);
            row.size->setRange(Recording::kInherit, Recording::kMaxSize);
            // The policy accepts -1 (inherit) or 1..kMaxSize, never 0. A spin box
            // cannot skip a value, so 0 snaps back to "inherit" instead of being
            // silently refused by the policy setter later.
            QSpinBox* sizeBox = row.size;
            connect(sizeBox, &QSpinBox::valueChanged, sizeBox, [sizeBox](int value) {
                if (value == 0) {
                    const QSignalBlocker blocker(sizeBox);
                    sizeBox->setValue(Recording::kInherit);
                }
            });
            row.size->setToolTip(
              tr("%1. \"Default\" uses the size the drawing tools already use.")
                .arg(Recording::AnnotationPolicy::sizeLabel(tool)));
            grid->addWidget(row.size, rowIndex, 3);
        }

        m_toolRows.append(row);
        ++rowIndex;
    }
    layout->addLayout(grid);

    // Pointer-keyed wiring: m_toolRows is complete before any of these fire.
    for (const auto tool : Recording::AnnotationPolicy::supportedTools()) {
        ToolRow* row = toolRow(tool);

        connect(row->hold, &QSpinBox::valueChanged, this, [this, tool](int value) {
            ToolRow* changed = toolRow(tool);
            if (changed == nullptr) {
                return;
            }
            Recording::ConfigHandlerAnnotationStore store;
            if (Recording::AnnotationPolicy::setHoldMs(store, tool, value)) {
                changed->configuredHold = value;
            } else {
                revertHold(*changed);
            }
        });

        connect(row->fade, &QSpinBox::valueChanged, this, [this, tool](int value) {
            ToolRow* changed = toolRow(tool);
            if (changed == nullptr) {
                return;
            }
            Recording::ConfigHandlerAnnotationStore store;
            if (Recording::AnnotationPolicy::setFadeMs(store, tool, value)) {
                changed->configuredFade = value;
            } else {
                revertFade(*changed);
            }
        });

        if (row->size == nullptr) {
            continue;
        }
        connect(row->size, &QSpinBox::valueChanged, this, [this, tool](int value) {
            ToolRow* changed = toolRow(tool);
            if (changed == nullptr) {
                return;
            }
            Recording::ConfigHandlerAnnotationStore store;
            if (Recording::AnnotationPolicy::setSize(store, tool, value)) {
                changed->configuredSize = value;
            } else {
                revertSize(*changed);
            }
        });
    }

    // The counter is the only tool whose strokes form one group, so its timing
    // rule is a setting of its own.
    auto* counterRow = new QWidget(m_toolSettings);
    auto* counterLayout = new QHBoxLayout(counterRow);
    counterLayout->setContentsMargins(0, 0, 0, 0);
    counterLayout->addWidget(new QLabel(tr("Circle counter bubbles:"), counterRow));
    m_counterMode = new QComboBox(counterRow);
    m_counterMode->setObjectName(QStringLiteral("counterMode"));
    m_counterMode->addItem(
      tr("Stay until another tool is used"),
      Recording::AnnotationPolicy::counterModeIdentifier(
        Recording::AnnotationCounterMode::HoldUntilToolChange));
    m_counterMode->addItem(
      tr("Fade the whole group from its first bubble"),
      Recording::AnnotationPolicy::counterModeIdentifier(
        Recording::AnnotationCounterMode::GroupFromFirstBubble));
    counterLayout->addWidget(m_counterMode, 1);
    layout->addWidget(counterRow);

    m_counterModeHint = new QLabel(m_toolSettings);
    m_counterModeHint->setObjectName(QStringLiteral("counterModeHint"));
    m_counterModeHint->setWordWrap(true);
    layout->addWidget(m_counterModeHint);

    connect(m_counterMode,
            QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int index) {
        if (index < 0) {
            return;
        }
        Recording::AnnotationCounterMode mode =
          Recording::AnnotationCounterMode::HoldUntilToolChange;
        if (!Recording::AnnotationPolicy::parseCounterMode(
              m_counterMode->itemData(index).toString(), &mode)) {
            return;
        }
        Recording::ConfigHandlerAnnotationStore store;
        if (!Recording::AnnotationPolicy::setCounterMode(store, mode)) {
            const QSignalBlocker blocker(m_counterMode);
            m_counterMode->setCurrentIndex(m_counterMode->findData(
              Recording::AnnotationPolicy::counterModeIdentifier(
                m_toolPolicy.counterMode())));
        }
        updateCounterModeHint();
    });

    return container;
}

void VideoConf::updateAnnotationToolsSection()
{
    ConfigHandler config;
    m_globalHoldMs = config.recordingHoldDuration();
    m_globalFadeMs = config.recordingFadeDuration();

    Recording::ConfigHandlerAnnotationStore store;
    m_toolPolicy = Recording::AnnotationPolicy::load(
      store, m_globalHoldMs, m_globalFadeMs);

    // A key the configuration does not know cannot be read or written without
    // tripping its unrecognised-setting guard, so the section says what is
    // missing instead of storing values nobody reads.
    const bool available = m_toolPolicy.isFullyAvailable();
    m_toolSettings->setEnabled(available);
    if (available) {
        m_toolSettingsStatus->clear();
        m_toolSettingsStatus->hide();
    } else {
        m_toolSettingsStatus->setText(
          tr("Inactive: these settings need configuration keys that are not "
             "registered yet (%1).")
            .arg(m_toolPolicy.unavailableKeys().join(QStringLiteral(", "))));
        m_toolSettingsStatus->show();
    }

    for (auto& row : m_toolRows) {
        const auto& settings = m_toolPolicy.settings(row.tool);
        row.configuredHold = settings.configured.holdMs;
        row.configuredFade = settings.configured.fadeMs;
        row.configuredSize = settings.configuredSize;

        const QSignalBlocker holdBlocker(row.hold);
        const QSignalBlocker fadeBlocker(row.fade);
        row.hold->setValue(row.configuredHold);
        row.fade->setValue(row.configuredFade);
        if (row.size != nullptr) {
            const QSignalBlocker sizeBlocker(row.size);
            row.size->setValue(row.configuredSize);
        }
    }

    const QSignalBlocker modeBlocker(m_counterMode);
    m_counterMode->setCurrentIndex(m_counterMode->findData(
      Recording::AnnotationPolicy::counterModeIdentifier(
        m_toolPolicy.counterMode())));

    updateInheritTexts();
    updateCounterModeHint();
}

void VideoConf::updateInheritTexts()
{
    // "Default" names the value it inherits, so the two global spin boxes above
    // stay the single source of truth for users who do not override anything.
    const QString holdText = tr("Default (%1 ms)").arg(m_globalHoldMs);
    const QString fadeText = tr("Default (%1 ms)").arg(m_globalFadeMs);
    for (auto& row : m_toolRows) {
        row.hold->setSpecialValueText(holdText);
        row.fade->setSpecialValueText(fadeText);
        if (row.size != nullptr) {
            row.size->setSpecialValueText(
              tr("Default (%1)").arg(Recording::drawingDefaultSize(row.tool)));
        }
    }
}

void VideoConf::updateCounterModeHint()
{
    switch (m_toolPolicy.counterMode()) {
        case Recording::AnnotationCounterMode::HoldUntilToolChange:
            m_counterModeHint->setText(
              tr("The bubbles keep full strength for as long as the circle "
                 "counter is the selected tool, then fade out over the fade "
                 "time once you switch to another tool. The hold time is not "
                 "used for them: they were already visible that long."));
            break;
        case Recording::AnnotationCounterMode::GroupFromFirstBubble:
            m_counterModeHint->setText(
              tr("Every bubble is timed from the first bubble of the group: the "
                 "group keeps full strength for the hold time, then fades out "
                 "over the fade time, even if the circle counter is still "
                 "selected."));
            break;
    }
}

void VideoConf::revertHold(ToolRow& row)
{
    const QSignalBlocker blocker(row.hold);
    row.hold->setValue(row.configuredHold);
}

void VideoConf::revertFade(ToolRow& row)
{
    const QSignalBlocker blocker(row.fade);
    row.fade->setValue(row.configuredFade);
}

void VideoConf::revertSize(ToolRow& row)
{
    if (row.size == nullptr) {
        return;
    }
    const QSignalBlocker blocker(row.size);
    row.size->setValue(row.configuredSize);
}
