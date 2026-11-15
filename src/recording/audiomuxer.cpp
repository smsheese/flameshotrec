// SPDX-License-Identifier: GPL-3.0-or-later
#include "audiomuxer.h"

#include "audiocapture.h"

#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>

namespace Recording {
namespace {

QString containerMuxer(const QString& container)
{
    const QString key = container.trimmed().toLower();
    if (key == QLatin1String("mkv")) return QStringLiteral("matroska");
    if (key == QLatin1String("mp4")) return QStringLiteral("mp4");
    if (key == QLatin1String("webm")) return QStringLiteral("webm");
    return {};
}

QString expectedSuffix(const QString& container)
{
    const QString key = container.trimmed().toLower();
    return key == QLatin1String("matroska") ? QStringLiteral("mkv") : key;
}

} // namespace

AudioMuxer::AudioMuxer(QObject* parent)
  : QObject(parent)
{}

AudioMuxer::~AudioMuxer()
{
    cancel();
}

bool AudioMuxer::validate(const Request& request, QString* error)
{
    if (error) error->clear();
    const QString codec = request.codec.trimmed().toLower();
    const QString container = request.container.trimmed().toLower();
    const QString requiredCodec = Audio::audioCodecForContainer(container);
    const QFileInfo video(request.videoPath);
    if (request.ffmpeg.trimmed().isEmpty() ||
        !QFileInfo(request.ffmpeg).isExecutable()) {
        if (error) *error = tr("The configured FFmpeg is not executable.");
        return false;
    }
    if (!video.isFile() || video.size() == 0) {
        if (error) *error = tr("The recorded video file is missing.");
        return false;
    }
    if (requiredCodec.isEmpty()) {
        if (error) {
            *error = tr("The %1 container cannot store audio.").arg(container);
        }
        return false;
    }
    if (codec != requiredCodec) {
        if (error) {
            *error = tr("The %1 container needs the %2 audio codec, not %3.")
                       .arg(container, requiredCodec, codec);
        }
        return false;
    }
    if (request.bitrateKbps < 32 || request.bitrateKbps > 512) {
        if (error) *error = tr("The audio bitrate must be between 32 and 512 kbit/s.");
        return false;
    }
    if (request.inputs.isEmpty()) {
        if (error) *error = tr("No captured audio is available to mix.");
        return false;
    }
    for (const auto& input : request.inputs) {
        const QFileInfo file(input.path);
        if (!file.isFile() || file.size() <= 0) {
            if (error) {
                *error = tr("A captured audio stream is empty: %1").arg(input.path);
            }
            return false;
        }
        if (input.sampleRate < 8000 || input.sampleRate > 192000 ||
            input.channels < 1 || input.channels > 8) {
            if (error) *error = tr("A captured audio stream has invalid parameters.");
            return false;
        }
    }
    const QFileInfo output(request.outputPath);
    if (output.suffix().compare(expectedSuffix(container),
                                Qt::CaseInsensitive) != 0) {
        if (error) {
            *error = tr("The output file extension must match the %1 container.")
                       .arg(container);
        }
        return false;
    }
    if (!QDir().mkpath(output.absolutePath())) {
        if (error) *error = tr("The output folder is not writable.");
        return false;
    }
    return true;
}

QStringList AudioMuxer::arguments(const Request& request)
{
    QStringList args{ QStringLiteral("-hide_banner"),
                      QStringLiteral("-nostdin"),
                      QStringLiteral("-loglevel"), QStringLiteral("error"),
                      QStringLiteral("-y"),
                      QStringLiteral("-i"), request.videoPath };
    for (const auto& input : request.inputs) {
        args << QStringLiteral("-f") << QStringLiteral("s16le")
             << QStringLiteral("-ar") << QString::number(input.sampleRate)
             << QStringLiteral("-ac") << QString::number(input.channels)
             << QStringLiteral("-i") << input.path;
    }
    QString graph;
    if (request.inputs.size() == 1) {
        graph = QStringLiteral("[1:a]apad[aout]");
    } else {
        QStringList pads;
        for (int index = 0; index < request.inputs.size(); ++index)
            pads << QStringLiteral("[%1:a]").arg(index + 1);
        // normalize=0 keeps the recorded levels instead of halving them; the
        // limiter only prevents clipping when both sources are loud.
        graph = pads.join(QString()) +
                QStringLiteral("amix=inputs=%1:duration=longest:normalize=0,"
                               "alimiter=limit=0.97:level=0,apad[aout]")
                  .arg(request.inputs.size());
    }
    args << QStringLiteral("-filter_complex") << graph
         << QStringLiteral("-map") << QStringLiteral("0:v:0")
         << QStringLiteral("-map") << QStringLiteral("[aout]")
         << QStringLiteral("-c:v") << QStringLiteral("copy")
         << QStringLiteral("-c:a") << request.codec
         << QStringLiteral("-b:a")
         << QStringLiteral("%1k").arg(request.bitrateKbps);
    if (request.container.compare(QLatin1String("mp4"), Qt::CaseInsensitive) == 0)
        args << QStringLiteral("-movflags") << QStringLiteral("+faststart");
    // The padded audio never ends, so the video length decides the duration.
    args << QStringLiteral("-shortest") << QStringLiteral("-f")
         << containerMuxer(request.container) << request.outputPath;
    return args;
}

bool AudioMuxer::start(const Request& request)
{
    if (m_running) return false;
    QString error;
    if (!validate(request, &error)) {
        emit failed(error);
        return false;
    }
    m_outputPath = request.outputPath;
    m_errorOutput.clear();
    m_cancelling = false;
    m_running = true;
    // One cause is reported once: FailedToStart used to be reported both by
    // errorOccurred() and by the waitForStarted() check below (review F10).
    m_startFailureReported = false;
    if (!m_process) {
        m_process = new QProcess(this);
        m_process->setProcessChannelMode(QProcess::SeparateChannels);
        connect(m_process, &QProcess::readyReadStandardError, this, [this]() {
            m_errorOutput += m_process->readAllStandardError();
            constexpr qsizetype maxErrorBytes = 8192;
            if (m_errorOutput.size() > maxErrorBytes) {
                m_errorOutput.remove(0, m_errorOutput.size() - maxErrorBytes);
            }
        });
        connect(m_process,
                qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
                &AudioMuxer::handleFinished);
        connect(m_process, &QProcess::errorOccurred, this,
                [this](QProcess::ProcessError processError) {
            if (processError != QProcess::FailedToStart) return;
            if (m_startFailureReported) return;
            m_startFailureReported = true;
            const QString message = m_process->errorString();
            m_running = false;
            m_cancelling = true;
            emit failed(tr("Could not start FFmpeg to mix the audio: %1")
                          .arg(message));
        });
    }
    m_process->start(request.ffmpeg, arguments(request));
    if (!m_process->waitForStarted(4000)) {
        if (m_startFailureReported) return false;
        m_startFailureReported = true;
        const QString message = m_process->errorString();
        m_running = false;
        m_cancelling = true;
        emit failed(tr("Could not start FFmpeg to mix the audio: %1").arg(message));
        return false;
    }
    return true;
}

void AudioMuxer::cancel()
{
    if (!m_running || !m_process) return;
    m_cancelling = true;
    m_running = false;
    m_process->kill();
    m_process->waitForFinished(1000);
}

void AudioMuxer::handleFinished(int exitCode, QProcess::ExitStatus status)
{
    m_running = false;
    if (m_cancelling) return;
    const QFileInfo output(m_outputPath);
    if (status == QProcess::NormalExit && exitCode == 0 && output.isFile() &&
        output.size() > 0) {
        emit succeeded(m_outputPath);
        return;
    }
    QString error = QString::fromLocal8Bit(m_errorOutput).trimmed();
    if (error.isEmpty()) error = m_process->errorString();
    emit failed(tr("Mixing the recorded audio failed: %1").arg(error));
}

} // namespace Recording
