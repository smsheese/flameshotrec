// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

#include <QProcess>

namespace Recording {

// Remuxes a finished video file with the captured raw audio sources. The video
// stream is copied, so the recording's exact frame timeline and duration are
// preserved; the audio is encoded to the codec the container requires.
class AudioMuxer final : public QObject
{
    Q_OBJECT
public:
    struct Input
    {
        QString path;
        int sampleRate{48000};
        int channels{2};
    };
    struct Request
    {
        QString ffmpeg;
        QString videoPath;
        QString outputPath;
        QString container;
        QString codec; // "aac" or "libopus"
        int bitrateKbps{128};
        QList<Input> inputs;
    };

    explicit AudioMuxer(QObject* parent = nullptr);
    ~AudioMuxer() override;

    static bool validate(const Request& request, QString* error);
    static QStringList arguments(const Request& request);

    bool start(const Request& request);
    void cancel();
    bool isRunning() const { return m_running; }

signals:
    void succeeded(const QString& outputPath);
    void failed(const QString& message);

private:
    void handleFinished(int exitCode, QProcess::ExitStatus status);

    QProcess* m_process{nullptr};
    QString m_outputPath;
    QByteArray m_errorOutput;
    bool m_running{false};
    bool m_cancelling{false};
    bool m_startFailureReported{false};
};

} // namespace Recording
