// SPDX-License-Identifier: GPL-3.0-or-later
#include "audiocapture.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>

#include <algorithm>

namespace Recording {
namespace {

constexpr int kChannels = 2;

QString trimmedValue(const QString& line, const QString& prefix)
{
    return line.trimmed().mid(prefix.size()).trimmed();
}

bool runCommand(const QString& program,
                const QStringList& arguments,
                int timeoutMs,
                QByteArray* output,
                QString* error)
{
    QProcess process;
    process.setProcessChannelMode(QProcess::SeparateChannels);
    process.start(program, arguments);
    if (!process.waitForStarted(timeoutMs)) {
        if (error) *error = process.errorString();
        return false;
    }
    if (!process.waitForFinished(timeoutMs)) {
        process.kill();
        process.waitForFinished(500);
        if (error) {
            *error = QStringLiteral("'%1' did not answer within %2 ms.")
                       .arg(program)
                       .arg(timeoutMs);
        }
        return false;
    }
    if (output) *output = process.readAllStandardOutput();
    if (process.exitStatus() != QProcess::NormalExit ||
        process.exitCode() != 0) {
        if (error) {
            const QString stderrText =
              QString::fromLocal8Bit(process.readAllStandardError()).trimmed();
            *error = stderrText.isEmpty() ? process.errorString() : stderrText;
        }
        return false;
    }
    return true;
}

QString joinSourceNames(const QList<AudioSourceInfo>& sources)
{
    QStringList names;
    for (const auto& source : sources) {
        if (!source.monitor) names.append(source.name);
    }
    return names.join(QStringLiteral(", "));
}

} // namespace

bool AudioCapabilities::usable(const QString& container) const
{
    const QString codec = Audio::audioCodecForContainer(container);
    if (codec.isEmpty() || !pulseInput) return false;
    return codec == QLatin1String("libopus") ? opusEncoder : aacEncoder;
}

namespace Audio {

QString pactlExecutable()
{
    return QStandardPaths::findExecutable(QStringLiteral("pactl"));
}

QList<AudioSourceInfo> parseSourceList(const QString& text)
{
    QList<AudioSourceInfo> sources;
    AudioSourceInfo current;
    bool inSource = false;
    const auto lines = text.split(QLatin1Char('\n'));
    for (const QString& raw : lines) {
        const QString line = raw.trimmed();
        if (line.startsWith(QLatin1String("Source #"))) {
            if (inSource && !current.name.isEmpty()) sources.append(current);
            current = AudioSourceInfo{};
            inSource = true;
            continue;
        }
        if (!inSource) continue;
        if (line.startsWith(QLatin1String("Name: "))) {
            current.name = trimmedValue(line, QStringLiteral("Name: "));
        } else if (line.startsWith(QLatin1String("Description: "))) {
            current.description =
              trimmedValue(line, QStringLiteral("Description: "));
        } else if (line.startsWith(QLatin1String("Monitor of Sink: "))) {
            current.monitor = true;
        }
    }
    if (inSource && !current.name.isEmpty()) sources.append(current);
    for (auto& source : sources) {
        // PipeWire reports monitor sources as well; the suffix keeps the
        // classification when the description block omits the sink line.
        if (!source.monitor &&
            source.name.endsWith(QLatin1String(".monitor"))) {
            source.monitor = true;
        }
        if (source.description.isEmpty()) source.description = source.name;
    }
    return sources;
}

QString parseDefaultSink(const QString& infoText)
{
    const auto lines = infoText.split(QLatin1Char('\n'));
    for (const QString& raw : lines) {
        const QString line = raw.trimmed();
        if (line.startsWith(QLatin1String("Default Sink: ")))
            return trimmedValue(line, QStringLiteral("Default Sink: "));
    }
    return {};
}

QString parseDefaultSource(const QString& infoText)
{
    const auto lines = infoText.split(QLatin1Char('\n'));
    for (const QString& raw : lines) {
        const QString line = raw.trimmed();
        if (line.startsWith(QLatin1String("Default Source: ")))
            return trimmedValue(line, QStringLiteral("Default Source: "));
    }
    return {};
}

QString monitorForSink(const QString& sink)
{
    const QString name = sink.trimmed();
    return name.isEmpty() ? QString() : name + QStringLiteral(".monitor");
}

QList<AudioSourceInfo> listSources(QString* error)
{
    if (error) error->clear();
    const QString pactl = pactlExecutable();
    if (pactl.isEmpty()) {
        if (error) {
            *error = QObject::tr(
              "The 'pactl' tool was not found. Audio device discovery needs "
              "PulseAudio's pactl (pulseaudio-utils), which also works with "
              "PipeWire.");
        }
        return {};
    }
    QByteArray output;
    QString failure;
    if (!runCommand(pactl,
                    { QStringLiteral("list"), QStringLiteral("sources") },
                    4000,
                    &output,
                    &failure)) {
        if (error) {
            *error = QObject::tr("Could not list audio devices with pactl: %1")
                       .arg(failure);
        }
        return {};
    }
    return parseSourceList(QString::fromLocal8Bit(output));
}

namespace {
QString pactlInfoField(const QString& key, QString* error)
{
    const QString pactl = pactlExecutable();
    if (pactl.isEmpty()) {
        if (error) {
            *error = QObject::tr(
              "The 'pactl' tool was not found. Audio device discovery needs "
              "PulseAudio's pactl (pulseaudio-utils), which also works with "
              "PipeWire.");
        }
        return {};
    }
    QByteArray output;
    QString failure;
    if (!runCommand(pactl, { QStringLiteral("info") }, 4000, &output, &failure)) {
        if (error) {
            *error = QObject::tr("Could not query the audio server: %1")
                       .arg(failure);
        }
        return {};
    }
    const QString info = QString::fromLocal8Bit(output);
    return key == QLatin1String("sink") ? parseDefaultSink(info)
                                        : parseDefaultSource(info);
}
} // namespace

QString defaultMonitorSource(QString* error)
{
    const QString sink = pactlInfoField(QStringLiteral("sink"), error);
    if (sink.isEmpty()) {
        if (error && error->isEmpty()) {
            *error = QObject::tr(
              "The audio server reports no default output device, so system "
              "audio cannot be captured.");
        }
        return {};
    }
    return monitorForSink(sink);
}

QString defaultInputSource(QString* error)
{
    const QString source = pactlInfoField(QStringLiteral("source"), error);
    if (source.isEmpty() && error && error->isEmpty()) {
        *error = QObject::tr(
          "The audio server reports no default microphone. Select a "
          "microphone in Configuration > Video.");
    }
    return source;
}

AudioCapabilities probeCapabilitiesUncached(const QString& ffmpeg)
{
    AudioCapabilities capabilities;
    if (ffmpeg.trimmed().isEmpty() || !QFileInfo(ffmpeg).isExecutable()) {
        capabilities.error = QObject::tr("The configured FFmpeg is not executable.");
        return capabilities;
    }
    QByteArray demuxers;
    QString failure;
    if (!runCommand(ffmpeg,
                    { QStringLiteral("-hide_banner"), QStringLiteral("-demuxers") },
                    6000,
                    &demuxers,
                    &failure)) {
        capabilities.error =
          QObject::tr("Could not query FFmpeg audio support: %1").arg(failure);
        return capabilities;
    }
    for (const QString& raw : QString::fromLocal8Bit(demuxers).split(QLatin1Char('\n'))) {
        const QString line = raw.trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('-'))) continue;
        // Device demuxers have an extra flag column ("D d pulse"), plain ones
        // only one ("D  matroska"), so accept the name in either position.
        const auto fields =
          line.split(QRegularExpression(QStringLiteral("\\s+")),
                     Qt::SkipEmptyParts);
        if (fields.size() >= 2 &&
            (fields.at(1) == QLatin1String("pulse") ||
             (fields.size() >= 3 && fields.at(2) == QLatin1String("pulse")))) {
            capabilities.pulseInput = true;
            break;
        }
    }
    QByteArray encoders;
    if (!runCommand(ffmpeg,
                    { QStringLiteral("-hide_banner"), QStringLiteral("-encoders") },
                    6000,
                    &encoders,
                    &failure)) {
        capabilities.error =
          QObject::tr("Could not query FFmpeg audio encoders: %1").arg(failure);
        return capabilities;
    }
    for (const QString& raw : QString::fromLocal8Bit(encoders).split(QLatin1Char('\n'))) {
        const QString line = raw.trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('-'))) continue;
        const auto fields = line.split(QRegularExpression(QStringLiteral("\\s+")),
                                       Qt::SkipEmptyParts);
        if (fields.size() < 2 || !fields.first().startsWith(QLatin1Char('A')))
            continue;
        const QString name = fields.at(1);
        if (name == QLatin1String("aac")) capabilities.aacEncoder = true;
        if (name == QLatin1String("libopus")) capabilities.opusEncoder = true;
    }
    if (!capabilities.pulseInput) {
        capabilities.error = QObject::tr(
          "This FFmpeg build has no PulseAudio input device, so audio cannot "
          "be captured. Use an FFmpeg built with PulseAudio support, or "
          "disable audio recording.");
    }
    return capabilities;
}

AudioCapabilities probeCapabilities(const QString& ffmpeg)
{
    // Two FFmpeg spawns cost ~0.1 s each and used to run on every recording
    // start, in front of the first captured frame (review F3). The answer only
    // changes when the binary does, so it is cached per path and timestamp.
    const QFileInfo binary(ffmpeg);
    static QHash<QString, AudioCapabilities> cache;
    const QString key = binary.absoluteFilePath() + QLatin1Char('@') +
                        QString::number(binary.lastModified().toSecsSinceEpoch());
    const auto cached = cache.constFind(key);
    if (cached != cache.constEnd()) {
        return *cached;
    }
    const AudioCapabilities probed = probeCapabilitiesUncached(ffmpeg);
    cache.insert(key, probed);
    return probed;
}

int sampleRateForQuality(const QString& quality)
{
    const QString key = quality.trimmed().toLower();
    if (key == QLatin1String("low")) return 32000;
    if (key == QLatin1String("medium")) return 44100;
    return 48000;
}

QStringList qualityKeys()
{
    return { QStringLiteral("low"),
             QStringLiteral("medium"),
             QStringLiteral("high") };
}

QString qualityDisplayName(const QString& quality)
{
    const QString key = quality.trimmed().toLower();
    if (key == QLatin1String("low")) return QObject::tr("Low (32 kHz)");
    if (key == QLatin1String("medium")) return QObject::tr("Medium (44.1 kHz)");
    return QObject::tr("High (48 kHz)");
}

QString audioCodecForContainer(const QString& container)
{
    const QString key = container.trimmed().toLower();
    if (key == QLatin1String("mp4") || key == QLatin1String("mkv"))
        return QStringLiteral("aac");
    if (key == QLatin1String("webm")) return QStringLiteral("libopus");
    return {};
}

bool resolveSources(const AudioRecordingOptions& options,
                    const QList<AudioSourceInfo>& available,
                    const QString& defaultMonitor,
                    const QString& defaultInput,
                    QList<AudioSourceSpec>* specs,
                    QString* error)
{
    if (!specs) return false;
    specs->clear();
    if (error) error->clear();
    if (!audioRequested(options)) return true;

    const auto known = [&available](const QString& name) {
        for (const auto& source : available) {
            if (source.name == name) return true;
        }
        return false;
    };
    const auto resolve = [&](const QString& configured,
                             const QString& fallback,
                             bool monitor,
                             const QString& label,
                             const QString& hint) -> bool {
        QString name = configured.trimmed();
        if (name.isEmpty()) name = fallback.trimmed();
        if (name.isEmpty()) {
            if (error) {
                *error = QObject::tr("Could not determine the %1 audio source. %2")
                           .arg(label, hint);
            }
            return false;
        }
        if (!available.isEmpty() && !known(name)) {
            if (error) {
                const QString names = monitor
                                        ? QObject::tr("no monitor sources were reported")
                                        : joinSourceNames(available);
                *error = QObject::tr(
                           "The %1 audio source '%2' is not available. %3")
                           .arg(label, name,
                                names.isEmpty()
                                  ? QObject::tr("No audio sources were reported.")
                                  : QObject::tr("Available sources: %1.").arg(names));
            }
            return false;
        }
        AudioSourceSpec spec;
        spec.label = label;
        spec.inputFormat = QStringLiteral("pulse");
        spec.inputUrl = name;
        spec.monitor = monitor;
        specs->append(spec);
        return true;
    };

    if (options.systemEnabled) {
        if (!resolve(options.systemSource,
                     defaultMonitor,
                     true,
                     QObject::tr("system"),
                     QObject::tr("The default output device has no monitor source. "
                                 "Select one in Configuration > Video, or disable "
                                 "system audio.")))
            return false;
    }
    if (options.microphoneEnabled) {
        if (!resolve(options.microphoneSource,
                     defaultInput,
                     false,
                     QObject::tr("microphone"),
                     QObject::tr("Select a microphone in Configuration > Video, or "
                                 "disable the microphone.")))
            return false;
    }
    return true;
}

bool resolveRequestedSources(const AudioRecordingOptions& options,
                             QList<AudioSourceSpec>* specs,
                             QString* error)
{
    if (!specs) return false;
    specs->clear();
    if (error) error->clear();
    if (!audioRequested(options)) return true;

    const bool systemFollowsDefault =
      options.systemEnabled && options.systemSource.trimmed().isEmpty();
    const bool micFollowsDefault =
      options.microphoneEnabled && options.microphoneSource.trimmed().isEmpty();
    QString defaultMonitor;
    QString defaultInput;
    QString probeError;
    if (systemFollowsDefault) {
        defaultMonitor = defaultMonitorSource(&probeError);
        if (defaultMonitor.isEmpty()) {
            if (error) *error = probeError;
            return false;
        }
    }
    if (micFollowsDefault) {
        defaultInput = defaultInputSource(&probeError);
        if (defaultInput.isEmpty()) {
            if (error) *error = probeError;
            return false;
        }
    }
    QList<AudioSourceInfo> available;
    if (systemFollowsDefault || micFollowsDefault) {
        QString listError;
        available = listSources(&listError);
        // A listing failure is not fatal: the default names already came from
        // the server and FFmpeg reports a precise error if they do not exist.
    }
    return resolveSources(
      options, available, defaultMonitor, defaultInput, specs, error);
}

} // namespace Audio

AudioRecorder::AudioRecorder(QObject* parent)
  : QObject(parent)
{}

AudioRecorder::~AudioRecorder()
{
    closeChannels();
}

bool AudioRecorder::start(const QString& ffmpeg,
                          const QList<AudioSourceSpec>& specs,
                          int sampleRate,
                          const QString& directory,
                          QString* error)
{
    if (error) error->clear();
    if (m_stopping || !m_channels.empty()) return false;
    if (specs.isEmpty()) {
        if (error) *error = tr("No audio source was requested.");
        return false;
    }
    if (sampleRate < 8000 || sampleRate > 192000) {
        if (error) *error = tr("Unsupported audio sample rate.");
        return false;
    }
    if (ffmpeg.trimmed().isEmpty() || !QFileInfo(ffmpeg).isExecutable()) {
        if (error) *error = tr("The configured FFmpeg is not executable.");
        return false;
    }
    QDir target(directory);
    if (target.path().isEmpty() || !target.exists()) {
        if (error) *error = tr("The audio workspace is not available.");
        return false;
    }
    m_ffmpeg = ffmpeg;
    m_directory = target.absolutePath();
    m_sampleRate = sampleRate;
    m_capturing = false;
    m_failed = false;
    m_stopping = false;

    for (int index = 0; index < specs.size(); ++index) {
        Channel channel;
        channel.spec = specs.at(index);
        channel.path =
          QDir(m_directory)
            .filePath(QStringLiteral("audio-%1.pcm").arg(index));
        channel.file = std::make_unique<QFile>(channel.path);
        if (!channel.file->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            if (error) {
                *error = tr("Could not create the audio workspace file %1.")
                           .arg(channel.path);
            }
            closeChannels();
            return false;
        }
        channel.process = std::make_unique<QProcess>();
        channel.process->setProcessChannelMode(QProcess::SeparateChannels);
        const int slot = index;
        connect(channel.process.get(), &QProcess::readyReadStandardOutput, this,
                [this, slot]() { readChannel(slot); });
        connect(channel.process.get(), &QProcess::readyReadStandardError, this,
                [this, slot]() {
            if (slot < 0 || slot >= m_channels.size()) return;
            Channel& target = m_channels[slot];
            target.errorOutput += target.process->readAllStandardError();
            constexpr qsizetype maxErrorBytes = 4096;
            if (target.errorOutput.size() > maxErrorBytes) {
                target.errorOutput.remove(
                  0, target.errorOutput.size() - maxErrorBytes);
            }
        });
        connect(channel.process.get(),
                qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
                [this, slot](int exitCode, QProcess::ExitStatus) {
            handleChannelFinished(slot, exitCode);
        });
        connect(channel.process.get(), &QProcess::errorOccurred, this,
                [this, slot](QProcess::ProcessError processError) {
            if (processError != QProcess::FailedToStart) return;
            if (slot < 0 || slot >= m_channels.size()) return;
            emitFailureOnce(tr("Audio capture for the %1 source could not start: %2")
                              .arg(m_channels[slot].spec.label,
                                   m_channels[slot].process->errorString()));
        });
        m_channels.push_back(std::move(channel));
    }

    for (int index = 0; index < m_channels.size(); ++index) {
        Channel& channel = m_channels[index];
        // -ac 2 keeps every source on the same layout so mixing is a plain
        // sample-sum without channel negotiation.
        QStringList inputArguments = channel.spec.extraInputArgs;
        inputArguments << QStringLiteral("-f") << channel.spec.inputFormat
                       << QStringLiteral("-i") << channel.spec.inputUrl;
        channel.process->start(
          ffmpeg,
          QStringList{ QStringLiteral("-hide_banner"),
                       QStringLiteral("-nostdin"),
                       QStringLiteral("-loglevel"),
                       QStringLiteral("error") } +
            inputArguments +
            QStringList{ QStringLiteral("-vn"),
                         QStringLiteral("-ac"), QString::number(kChannels),
                         QStringLiteral("-ar"), QString::number(m_sampleRate),
                         QStringLiteral("-f"), QStringLiteral("s16le"),
                         QStringLiteral("-acodec"),
                         QStringLiteral("pcm_s16le"),
                         QStringLiteral("pipe:1") });
    }
    for (int index = 0; index < m_channels.size(); ++index) {
        Channel& channel = m_channels[index];
        if (!channel.process->waitForStarted(4000)) {
            if (error) {
                *error = tr("Audio capture for the %1 source could not start: %2")
                           .arg(channel.spec.label, channel.process->errorString());
            }
            closeChannels();
            return false;
        }
    }
    // A missing or busy device fails right after start. Waiting for the first
    // sample turns that into a clear startup error instead of a recording that
    // silently loses its requested audio.
    for (int index = 0; index < m_channels.size(); ++index) {
        Channel& channel = m_channels[index];
        channel.process->waitForReadyRead(150);
        if (channel.process->state() == QProcess::NotRunning) {
            const QString details =
              QString::fromLocal8Bit(channel.errorOutput).trimmed();
            if (error) {
                *error =
                  tr("Could not capture audio from '%1' (%2): %3")
                    .arg(channel.spec.inputUrl, channel.spec.label,
                         details.isEmpty() ? channel.process->errorString()
                                           : details);
            }
            closeChannels();
            return false;
        }
        readChannel(index);
    }
    return true;
}

void AudioRecorder::setCapturing(bool capturing)
{
    if (m_stopping || m_channels.empty()) return;
    if (capturing == m_capturing) return;
    if (capturing) {
        // Everything buffered so far belongs to the paused interval.
        discardPending();
        m_capturing = true;
    } else {
        // Keep the tail that arrived while the recording was still active.
        for (int index = 0; index < m_channels.size(); ++index)
            readChannel(index);
        m_capturing = false;
    }
}

void AudioRecorder::readChannel(int index)
{
    if (index < 0 || index >= m_channels.size()) return;
    Channel& channel = m_channels[index];
    const QByteArray data = channel.process->readAllStandardOutput();
    if (data.isEmpty()) return;
    if (!m_capturing || !channel.file) return;
    const qint64 written = channel.file->write(data);
    if (written != data.size()) {
        emitFailureOnce(tr("Could not write captured audio: %1")
                          .arg(channel.file->errorString()));
        return;
    }
    channel.bytes += written;
}

void AudioRecorder::discardPending()
{
    for (auto& channel : m_channels) {
        if (channel.process) channel.process->readAllStandardOutput();
    }
}

void AudioRecorder::handleChannelFinished(int index, int exitCode)
{
    if (m_stopping || index < 0 || index >= m_channels.size()) return;
    Channel& channel = m_channels[index];
    readChannel(index);
    const QString details = QString::fromLocal8Bit(channel.errorOutput).trimmed();
    emitFailureOnce(tr("Audio capture for the %1 source stopped unexpectedly "
                       "(exit code %2)%3")
                      .arg(channel.spec.label)
                      .arg(exitCode)
                      .arg(details.isEmpty() ? QString()
                                             : QStringLiteral(": ") + details));
}

void AudioRecorder::emitFailureOnce(const QString& message)
{
    if (m_failed) return;
    m_failed = true;
    emit failed(message);
}

void AudioRecorder::stop()
{
    if (m_stopping || m_channels.empty()) return;
    m_stopping = true;
    for (int index = 0; index < m_channels.size(); ++index)
        readChannel(index);
    for (auto& channel : m_channels) {
        if (!channel.process) continue;
        channel.process->closeWriteChannel();
        channel.process->terminate();
    }
    for (auto& channel : m_channels) {
        if (!channel.process) continue;
        if (!channel.process->waitForFinished(1500)) {
            channel.process->kill();
            channel.process->waitForFinished(500);
        }
    }
    for (int index = 0; index < m_channels.size(); ++index)
        readChannel(index);
    for (auto& channel : m_channels) {
        if (!channel.file) continue;
        channel.file->flush();
        channel.file->close();
    }
}

void AudioRecorder::closeChannels()
{
    m_capturing = false;
    m_stopping = true;
    for (auto& channel : m_channels) {
        if (channel.process) {
            channel.process->kill();
            channel.process->waitForFinished(500);
        }
        if (channel.file) {
            channel.file->flush();
            channel.file->close();
        }
    }
}

qint64 AudioRecorder::capturedBytes(int index) const
{
    if (index < 0 || index >= m_channels.size()) return 0;
    return m_channels.at(index).bytes;
}

qint64 AudioRecorder::capturedMilliseconds(int index) const
{
    const int bytesPerFrame = kChannels * int(sizeof(qint16));
    if (m_sampleRate <= 0) return 0;
    return capturedBytes(index) * 1000 / (bytesPerFrame * m_sampleRate);
}

qint64 AudioRecorder::capturedMilliseconds() const
{
    qint64 milliseconds = 0;
    for (int index = 0; index < m_channels.size(); ++index)
        milliseconds = std::max(milliseconds, capturedMilliseconds(index));
    return milliseconds;
}

QString AudioRecorder::filePath(int index) const
{
    if (index < 0 || index >= m_channels.size()) return {};
    return m_channels.at(index).path;
}

QString AudioRecorder::label(int index) const
{
    if (index < 0 || index >= m_channels.size()) return {};
    return m_channels.at(index).spec.label;
}

} // namespace Recording
