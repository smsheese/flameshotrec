// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QList>
#include <QWidget>

#include "recording/annotationpolicy.h"
#include "recording/audiocapture.h"

class QCheckBox;
class QComboBox;
class QGroupBox;
class QLabel;
class QLineEdit;
class QProcess;
class QSpinBox;
class QTimer;
class QVBoxLayout;

class VideoConf : public QWidget
{
    Q_OBJECT
public:
    explicit VideoConf(QWidget* parent = nullptr);
public slots:
    void updateComponents();
private:
    // One row of the "Annotation tools" section: the per-tool lifetime and, for
    // the tools that have one, the per-tool size level.
    struct ToolRow
    {
        Recording::AnnotationTool tool;
        QSpinBox* hold{nullptr};
        QSpinBox* fade{nullptr};
        QSpinBox* size{nullptr};
        int configuredHold{Recording::kInherit};
        int configuredFade{Recording::kInherit};
        int configuredSize{Recording::kInherit};
    };

    void updateEncoderOptions();
    void populateAudioDevices();
    void queryAudioDevices();
    void applyAudioSources(const QList<Recording::AudioSourceInfo>& sources,
                           const QString& error);
    void updateAudioControls();
    QWidget* createAnnotationToolsSection(QWidget* parent);
    void updateAnnotationToolsSection();
    void updateInheritTexts();
    void updateCounterModeHint();
    ToolRow* toolRow(Recording::AnnotationTool tool);
    void revertHold(ToolRow& row);
    void revertFade(ToolRow& row);
    void revertSize(ToolRow& row);
    QLineEdit* m_ffmpegPath;
    QLineEdit* m_videoPath;
    QLineEdit* m_filenamePattern;
    QSpinBox* m_frameRate;
    QSpinBox* m_holdDuration;
    QSpinBox* m_fadeDuration;
    QSpinBox* m_bitrate;
    QSpinBox* m_quality;
    QComboBox* m_container;
    QComboBox* m_codec;
    QComboBox* m_rateControl;
    QCheckBox* m_audioSystem;
    QCheckBox* m_audioMic;
    QComboBox* m_audioSystemSource;
    QComboBox* m_audioMicSource;
    QComboBox* m_audioQuality;
    QSpinBox* m_audioBitrate;
    QLabel* m_audioStatus;
    // Device discovery runs through the event loop: `pactl` can block for
    // seconds when the sound server is wedged, and this widget is rebuilt on
    // every configuration write (review F4).
    QTimer* m_audioRefreshTimer{nullptr};
    QProcess* m_audioQuery{nullptr};
    bool m_audioRefreshPending{false};
    // Annotation tools section (bottom of the tab).
    QGroupBox* m_toolSettings{nullptr};
    QLabel* m_toolSettingsStatus{nullptr};
    QComboBox* m_counterMode{nullptr};
    QLabel* m_counterModeHint{nullptr};
    QList<ToolRow> m_toolRows;
    Recording::AnnotationPolicy m_toolPolicy;
    int m_globalHoldMs{Recording::kDefaultHoldMs};
    int m_globalFadeMs{Recording::kDefaultFadeMs};
};
