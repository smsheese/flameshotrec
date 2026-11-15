// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "audiocapture.h"
#include "audiomuxer.h"
#include "x11recordingbackend.h"
#include <QElapsedTimer>
#include <QTemporaryDir>
#include <memory>

namespace Recording {
// Owns the temporary recording and publishes it only after successful muxing.
// Cancelling can remove only this session's private temporary directory.
class CaptureRecordingSession final : public QObject
{
    Q_OBJECT
public:
    // `Copy` keeps the finished recording in the private staging directory and
    // hands its file URI to the caller: the configured video folder is neither
    // created nor written on that path.
    enum class Action { Save, Open, Upload, Copy };
    Q_ENUM(Action)
    explicit CaptureRecordingSession(QObject* parent = nullptr);
    ~CaptureRecordingSession() override;
    bool start(const QString& ffmpeg, const QRect& physicalRegion,
               const QString& destination, const RecordingOptions& options);
    // `stagingFile` is a unique, not yet existing path inside the private
    // staging directory. The finished recording is kept there whatever the
    // action was, and only a Save/Open/Upload moves it to the destination at
    // publish time. An empty value keeps a compatibility path for callers that
    // do not stage: the encoder then writes next to `destination`.
    bool start(const QString& ffmpeg, const QRect& physicalRegion,
               const QString& destination, const QString& stagingFile,
               const RecordingOptions& options);
    bool togglePause();
    void finish(Action action = Action::Save);
    void cancel();
    X11RecordingBackend::State state() const { return m_backend.state(); }
    qint64 activeMilliseconds() const;
    // Why the last start() failed, when the cause is specific (missing audio
    // device, unavailable sound server, unsupported FFmpeg build).
    QString lastError() const { return m_startupError; }
    bool hasAudio() const { return m_audioRequested; }
signals:
    void stateChanged(Recording::X11RecordingBackend::State state);
    void saved(const QString& path, Recording::CaptureRecordingSession::Action action);
    void failed(const QString& message);
    void warning(const QString& message);
private:
    bool startAudio(const QString& ffmpeg, const RecordingOptions& options);
    void handleAudioFailure(const QString& message);
    void muxAudio(const QString& videoPath);
    void publish(const QString& path);
    // Publishes the finished file under the requested destination name, creating
    // that folder only now: a recording that is copied instead of saved must not
    // depend on (or create) the configured video folder. Never replaces an
    // existing file; a taken name becomes `<base>_1`, `<base>_2`, ...
    QString moveToDestination(const QString& path);
    // Publishes the finished file as the private staging file (the name the
    // clipboard offers) and returns its path there.
    QString moveToStaging(const QString& path);
    QString keepTemporaryFiles();
    void reportFailure(const QString& message);

    X11RecordingBackend m_backend;
    AudioRecorder m_audio;
    AudioMuxer m_muxer;
    std::unique_ptr<QTemporaryDir> m_temporary;
    QString m_destination;
    QString m_stagingFile;
    // The folder the encoder actually writes in: the private staging directory,
    // or the destination folder on the compatibility path for callers that do
    // not stage.
    QString m_encoderDirectory;
    bool m_encodedInStaging{false};
    QString m_ffmpeg;
    QString m_container;
    QString m_startupError;
    // Why the last publish attempt failed, so the failure message names a cause
    // instead of only the path (a taken name, an unwritable folder, a filesystem
    // that cannot rename across devices).
    QString m_publishError;
    AudioRecordingOptions m_audioOptions;
    Action m_action{Action::Save};
    bool m_cancelled{false};
    bool m_finishing{false};
    bool m_audioRequested{false};
    bool m_audioStarted{false};
    bool m_startingAudio{false};
    QString m_pendingAudioFailure;
    bool m_failureReported{false};
    QElapsedTimer m_segment;
    qint64 m_elapsed{0};
    bool m_running{false};
};
}
