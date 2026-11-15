// SPDX-License-Identifier: GPL-3.0-or-later
// Synthetic audio tests: no real microphone, speaker or sound server is used.
// Source parsing, source resolution and duration accounting are exercised with
// fixtures; the end-to-end test drives FFmpeg's lavfi sine generators and a
// generated test video.
#include "audiocapture.h"
#include "audiomuxer.h"
#include "capturerecordingsession.h"

#include <QtTest>

#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>

#include <csignal>
#include <sys/stat.h>

using namespace Recording;

namespace {

constexpr int kStartTimeoutMs = 5000;
constexpr int kToolTimeoutMs = 120000;
constexpr int kKillTimeoutMs = 1000;

QString runTool(const QString& program,
                const QStringList& arguments,
                bool* succeeded = nullptr)
{
    QProcess process;
    process.setProcessChannelMode(QProcess::SeparateChannels);
    process.start(program, arguments);
    bool finished = false;
    if (process.waitForStarted(kStartTimeoutMs) &&
        process.waitForFinished(kToolTimeoutMs)) {
        finished = process.exitStatus() == QProcess::NormalExit &&
                   process.exitCode() == 0;
    } else {
        process.kill();
        process.waitForFinished(kKillTimeoutMs);
    }
    const QString output = QString::fromLocal8Bit(process.readAllStandardOutput());
    // Never dereference a caller's optional out-parameter when it is absent.
    if (succeeded) {
        *succeeded = finished;
    }
    if (!finished) {
        qWarning("tool failed: %s %s\n%s", qPrintable(program),
                 qPrintable(arguments.join(QLatin1Char(' '))),
                 qPrintable(QString::fromLocal8Bit(process.readAllStandardError())));
    }
    return output;
}

QString ffmpegPath()
{
    return QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
}

QString ffprobePath()
{
    return QStandardPaths::findExecutable(QStringLiteral("ffprobe"));
}

bool hasEncoder(const QString& ffmpeg, const QString& name)
{
    bool ok = false;
    const QString encoders = runTool(ffmpeg, { QStringLiteral("-hide_banner"),
                                               QStringLiteral("-encoders") },
                                     &ok);
    if (!ok) return false;
    for (const QString& line : encoders.split(QLatin1Char('\n'))) {
        const auto fields = line.trimmed().split(QRegularExpression(QStringLiteral("\\s+")),
                                                 Qt::SkipEmptyParts);
        if (fields.size() >= 2 && fields.at(1) == name) return true;
    }
    return false;
}

QString ffprobeField(const QString& ffprobe, const QString& path,
                     const QString& entries)
{
    bool ok = false;
    const QString output =
      runTool(ffprobe,
              { QStringLiteral("-v"), QStringLiteral("error"),
                QStringLiteral("-select_streams"), QStringLiteral("a:0"),
                QStringLiteral("-show_entries"), entries,
                QStringLiteral("-of"), QStringLiteral("default=nw=1"), path },
              &ok);
    if (!ok) return {};
    for (const QString& line : output.split(QLatin1Char('\n'))) {
        const auto parts = line.trimmed().split(QLatin1Char('='));
        if (parts.size() == 2 && entries.contains(parts.first()))
            return parts.at(1);
    }
    return {};
}

QString formatDuration(const QString& ffprobe, const QString& path)
{
    bool ok = false;
    const QString output =
      runTool(ffprobe,
              { QStringLiteral("-v"), QStringLiteral("error"),
                QStringLiteral("-show_entries"), QStringLiteral("format=duration"),
                QStringLiteral("-of"), QStringLiteral("default=nw=1:nk=1"), path },
              &ok);
    return ok ? output.trimmed() : QString();
}

QByteArray syntheticSourceList()
{
    return QByteArrayLiteral(
      "Source #44\n"
      "\tState: SUSPENDED\n"
      "\tName: alsa_output.pci-0000_00_1f.3.analog-stereo.monitor\n"
      "\tDescription: Monitor of Built-in Audio Analog Stereo\n"
      "\tDriver: PipeWire\n"
      "\tMonitor of Sink: alsa_output.pci-0000_00_1f.3.analog-stereo\n"
      "\tProperties:\n"
      "\t\tdevice.description = \"Built-in Audio\"\n"
      "Ports:\n"
      "Source #45\n"
      "\tState: SUSPENDED\n"
      "\tName: alsa_input.pci-0000_00_1f.3.analog-stereo\n"
      "\tDescription: Built-in Audio Analog Stereo\n"
      "\tDriver: PipeWire\n"
      "\tProperties:\n"
      "\t\tdevice.description = \"Built-in Audio\"\n"
      "Source #46\n"
      "\tState: RUNNING\n"
      "\tName: usb_mic_stereo\n"
      "\tDescription: USB Microphone\n"
      "\tDriver: PipeWire\n");
}

} // namespace

namespace {

// Fake FFmpeg for the session tests. Real FFmpeg handles the encoder and mux
// invocations (their arguments are genuine), X11 frames are synthesised, and a
// requested PulseAudio source is replaced by a realtime synthetic sine wave.
QString writeSessionFfmpegFixture(const QTemporaryDir& directory)
{
    const QString path = directory.filePath(QStringLiteral("ffmpeg"));
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return {};
    file.write(
      "#!/usr/bin/python3\n"
      "import os, sys, time\n"
      "args = sys.argv[1:]\n"
      "if 'x11grab' in args:\n"
      "    frame = bytes([127] * 32)\n"
      "    out = sys.stdout.buffer\n"
      "    while True:\n"
      "        out.write(frame)\n"
      "        out.flush()\n"
      "        time.sleep(0.02)\n"
      "if '-f' in args and 'pulse' in args:\n"
      "    rewritten = []\n"
      "    index = 0\n"
      "    while index < len(args):\n"
      "        if args[index] == '-f' and args[index + 1] == 'pulse':\n"
      "            rewritten += ['-f', 'lavfi']\n"
      "            index += 2\n"
      "        elif args[index] == '-i':\n"
      "            rewritten += ['-i', 'sine=frequency=440:sample_rate=48000']\n"
      "            index += 2\n"
      "        else:\n"
      "            rewritten.append(args[index])\n"
      "            index += 1\n"
      "    args = ['-re'] + rewritten\n"
      "os.execv('/usr/bin/ffmpeg', ['ffmpeg'] + args)\n");
    file.close();
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                        QFileDevice::ExeOwner);
    return path;
}

// Emits 48 kHz stereo PCM at wall-clock speed, like a capture device.
QString writeLiveSourceFfmpeg(const QTemporaryDir& directory)
{
    const QString path = directory.filePath(QStringLiteral("live-ffmpeg"));
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        return {};
    }
    file.write(
      "#!/usr/bin/python3\n"
      "import sys, time\n"
      "rate = 192000  # bytes per second: 48 kHz, stereo, 16 bit\n"
      "chunk = 3840   # 20 ms\n"
      "start = time.monotonic()\n"
      "sent = 0\n"
      "out = sys.stdout.buffer\n"
      "payload = bytes([1]) * chunk\n"
      "while True:\n"
      "    due = int((time.monotonic() - start) * rate)\n"
      "    while sent + chunk <= due:\n"
      "        out.write(payload)\n"
      "        sent += chunk\n"
      "    out.flush()\n"
      "    time.sleep(0.005)\n");
    file.close();
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                        QFileDevice::ExeOwner);
    return path;
}

constexpr int kTestFrameRate = 30;
constexpr int kTestAudioBitrateKbps = 96;

RecordingOptions audioOptionsForTest()
{
    RecordingOptions options;
    options.frameRate = kTestFrameRate;
    options.codec = QStringLiteral("libx264");
    options.container = QStringLiteral("mp4");
    // Explicit names keep the test independent of the sound server; the fake
    // FFmpeg substitutes a synthetic realtime source.
    options.audio.systemEnabled = true;
    options.audio.systemSource = QStringLiteral("test_monitor");
    options.audio.microphoneEnabled = true;
    options.audio.microphoneSource = QStringLiteral("test_microphone");
    options.audio.quality = QStringLiteral("high");
    options.audio.bitrateKbps = kTestAudioBitrateKbps;
    return options;
}

} // namespace

class AudioCaptureTest : public QObject
{
    Q_OBJECT
private slots:
    void parsesPulseSourceList()
    {
        const auto sources = Audio::parseSourceList(
          QString::fromLatin1(syntheticSourceList()));
        QCOMPARE(sources.size(), 3);
        QCOMPARE(sources.at(0).name,
                 QStringLiteral("alsa_output.pci-0000_00_1f.3.analog-stereo.monitor"));
        QVERIFY(sources.at(0).monitor);
        QCOMPARE(sources.at(0).description,
                 QStringLiteral("Monitor of Built-in Audio Analog Stereo"));
        QCOMPARE(sources.at(1).name,
                 QStringLiteral("alsa_input.pci-0000_00_1f.3.analog-stereo"));
        QVERIFY(!sources.at(1).monitor);
        QCOMPARE(sources.at(2).name, QStringLiteral("usb_mic_stereo"));
        QVERIFY(!sources.at(2).monitor);
    }

    void parsesDefaultDevices()
    {
        const QString info = QStringLiteral(
          "Server String: /run/user/1000/pulse/native\n"
          "Server Name: PulseAudio (on PipeWire 1.4.2)\n"
          "Default Sink: alsa_output.pci-0000_00_1f.3.analog-stereo\n"
          "Default Source: alsa_input.pci-0000_00_1f.3.analog-stereo\n");
        QCOMPARE(Audio::parseDefaultSink(info),
                 QStringLiteral("alsa_output.pci-0000_00_1f.3.analog-stereo"));
        QCOMPARE(Audio::parseDefaultSource(info),
                 QStringLiteral("alsa_input.pci-0000_00_1f.3.analog-stereo"));
        QCOMPARE(Audio::monitorForSink(QStringLiteral("speaker_sink")),
                 QStringLiteral("speaker_sink.monitor"));
        QVERIFY(Audio::monitorForSink(QString()).isEmpty());
        // A server without devices must not yield a bogus monitor name.
        QVERIFY(Audio::parseDefaultSink(QStringLiteral("Server Name: x\n")).isEmpty());
    }

    void audioQualitySelectsSampleRate()
    {
        QCOMPARE(Audio::sampleRateForQuality(QStringLiteral("low")), 32000);
        QCOMPARE(Audio::sampleRateForQuality(QStringLiteral("medium")), 44100);
        QCOMPARE(Audio::sampleRateForQuality(QStringLiteral("high")), 48000);
        QCOMPARE(Audio::sampleRateForQuality(QStringLiteral("unknown")), 48000);
        QCOMPARE(Audio::qualityKeys().size(), 3);
    }

    void containerSelectsAudioCodec()
    {
        QCOMPARE(Audio::audioCodecForContainer(QStringLiteral("mp4")),
                 QStringLiteral("aac"));
        QCOMPARE(Audio::audioCodecForContainer(QStringLiteral("mkv")),
                 QStringLiteral("aac"));
        QCOMPARE(Audio::audioCodecForContainer(QStringLiteral("webm")),
                 QStringLiteral("libopus"));
        QVERIFY(Audio::audioCodecForContainer(QStringLiteral("gif")).isEmpty());
    }

    void audioDefaultsAreOffButConfigFriendly()
    {
        RecordingOptions options;
        QVERIFY(!options.audio.systemEnabled);
        QVERIFY(!options.audio.microphoneEnabled);
        QVERIFY(!audioRequested(options.audio));
        QCOMPARE(options.audio.quality, QStringLiteral("high"));
        QCOMPARE(options.audio.bitrateKbps, 128);
        // A requested source without a usable container is rejected instead of
        // being recorded silently.
        AudioMuxer::Request request;
        request.ffmpeg = QStringLiteral("/bin/true");
        request.videoPath = QStringLiteral("/nonexistent/video.mp4");
        request.container = QStringLiteral("webm");
        request.codec = QStringLiteral("aac");
        QString error;
        QVERIFY(!AudioMuxer::validate(request, &error));
        QVERIFY(!error.isEmpty());
    }

    void resolvesSelectedSources()
    {
        const auto available = Audio::parseSourceList(
          QString::fromLatin1(syntheticSourceList()));
        // The sound server's defaults come from `pactl info`; both are real
        // entries of the listing below.
        const QString defaultMonitor =
          QStringLiteral("alsa_output.pci-0000_00_1f.3.analog-stereo.monitor");
        const QString defaultInput =
          QStringLiteral("alsa_input.pci-0000_00_1f.3.analog-stereo");
        RecordingOptions options;
        QList<AudioSourceSpec> specs;
        QString error;

        // Nothing requested: nothing resolved, and no error.
        QVERIFY(Audio::resolveSources(options.audio, available, defaultMonitor,
                                      defaultInput, &specs, &error));
        QVERIFY(specs.isEmpty());
        QVERIFY(error.isEmpty());

        // Defaults: system audio enabled follows the default output monitor;
        // the microphone stays off by default.
        options.audio.systemEnabled = true;
        QVERIFY(Audio::resolveSources(options.audio, available, defaultMonitor,
                                      defaultInput, &specs, &error));
        QCOMPARE(specs.size(), 1);
        QCOMPARE(specs.at(0).inputUrl, defaultMonitor);
        QCOMPARE(specs.at(0).inputFormat, QStringLiteral("pulse"));
        QVERIFY(specs.at(0).monitor);

        // Explicit selection of both sources.
        options.audio.microphoneEnabled = true;
        options.audio.systemSource =
          QStringLiteral("alsa_output.pci-0000_00_1f.3.analog-stereo.monitor");
        options.audio.microphoneSource = QStringLiteral("usb_mic_stereo");
        QVERIFY(Audio::resolveSources(options.audio, available, defaultMonitor,
                                      defaultInput, &specs, &error));
        QCOMPARE(specs.size(), 2);
        QCOMPARE(specs.at(0).inputUrl,
                 QStringLiteral("alsa_output.pci-0000_00_1f.3.analog-stereo.monitor"));
        QCOMPARE(specs.at(1).inputUrl, QStringLiteral("usb_mic_stereo"));
        QVERIFY(!specs.at(1).monitor);

        // A requested source the server does not report is an error, not a
        // silent fallback to something else.
        options.audio.microphoneSource = QStringLiteral("unplugged_mic");
        QVERIFY(!Audio::resolveSources(options.audio, available, defaultMonitor,
                                       defaultInput, &specs, &error));
        QVERIFY(error.contains(QStringLiteral("unplugged_mic")));

        // Without a default output the system source cannot be resolved.
        options.audio.microphoneSource.clear();
        options.audio.systemSource.clear();
        QVERIFY(!Audio::resolveSources(options.audio, available, QString(),
                                       defaultInput, &specs, &error));
        QVERIFY(!error.isEmpty());
    }

    void muxerArgumentsFollowContainerRules()
    {
        AudioMuxer::Request request;
        request.videoPath = QStringLiteral("/tmp/video.mp4");
        request.outputPath = QStringLiteral("/tmp/mixed.mp4");
        request.container = QStringLiteral("mp4");
        request.codec = QStringLiteral("aac");
        request.inputs = { { QStringLiteral("/tmp/a.pcm"), 48000, 2 } };
        const QStringList single = AudioMuxer::arguments(request);
        QVERIFY(single.contains(QStringLiteral("-c:v")));
        QCOMPARE(single.at(single.indexOf(QStringLiteral("-c:v")) + 1),
                 QStringLiteral("copy"));
        QVERIFY(single.join(QLatin1Char(' ')).contains(QStringLiteral("apad")));
        QVERIFY(single.contains(QStringLiteral("-shortest")));
        QVERIFY(single.join(QLatin1Char(' ')).contains(QStringLiteral("faststart")));

        request.inputs.append({ QStringLiteral("/tmp/b.pcm"), 48000, 2 });
        const QStringList mixed = AudioMuxer::arguments(request);
        const QString graph = mixed.at(mixed.indexOf(QStringLiteral("-filter_complex")) + 1);
        QVERIFY(graph.contains(QStringLiteral("[1:a][2:a]amix=inputs=2")));
        QVERIFY(graph.contains(QStringLiteral("normalize=0")));
        QVERIFY(graph.endsWith(QStringLiteral("apad[aout]")));
    }

    void refusesAudioTheContainerCannotStore()
    {
        QTemporaryDir workspace;
        QVERIFY(workspace.isValid());
        const QString ffmpeg = ffmpegPath();
        if (ffmpeg.isEmpty()) QSKIP("FFmpeg is required for the muxer fixtures.");
        const QString video = workspace.filePath(QStringLiteral("video.mp4"));
        QFile videoFile(video);
        QVERIFY(videoFile.open(QIODevice::WriteOnly));
        videoFile.write("synthetic video payload");
        videoFile.close();
        const QString pcm = workspace.filePath(QStringLiteral("audio-0.pcm"));
        QFile pcmFile(pcm);
        QVERIFY(pcmFile.open(QIODevice::WriteOnly));
        pcmFile.write(QByteArray(48000, char(1)));
        pcmFile.close();

        AudioMuxer::Request request;
        request.ffmpeg = ffmpeg;
        request.videoPath = video;
        request.outputPath = workspace.filePath(QStringLiteral("mixed.mp4"));
        request.container = QStringLiteral("mp4");
        request.codec = QStringLiteral("libopus");
        request.inputs = { { pcm, 48000, 2 } };
        QString error;
        QVERIFY(!AudioMuxer::validate(request, &error));
        QVERIFY(error.contains(QStringLiteral("aac")));

        // Wrong sample rate and an extension that does not match the container
        // are refused as well.
        request.codec = QStringLiteral("aac");
        request.inputs.first().sampleRate = 100;
        QVERIFY(!AudioMuxer::validate(request, &error));
        QVERIFY(!error.isEmpty());
        request.inputs.first().sampleRate = 48000;
        request.outputPath = workspace.filePath(QStringLiteral("mixed.webm"));
        QVERIFY(!AudioMuxer::validate(request, &error));
        QVERIFY(!error.isEmpty());
    }

    void capturesLiveSourcesWithoutPausedAudio()
    {
        const QString ffmpeg = ffmpegPath();
        const QString ffprobe = ffprobePath();
        if (ffmpeg.isEmpty()) QSKIP("FFmpeg is required for synthetic audio.");
        if (!QFileInfo::exists(QStringLiteral("/usr/bin/python3")))
            QSKIP("Python is required for the live source fixture.");
        QTemporaryDir workspace;
        QVERIFY(workspace.isValid());
        // The fixture behaves like a capture device: it produces 48 kHz stereo
        // PCM at wall-clock speed and never catches up on its own, so a paused
        // interval can only disappear if the recorder really drops it.
        const QString live = writeLiveSourceFfmpeg(workspace);
        QVERIFY(!live.isEmpty());
        const auto makeSpec = [](const QString& label) {
            AudioSourceSpec source;
            source.label = label;
            source.inputFormat = QStringLiteral("test");
            source.inputUrl = label;
            source.monitor = label == QLatin1String("system");
            return source;
        };
        AudioRecorder recorder;
        QString error;
        QVERIFY2(recorder.start(live,
                                { makeSpec(QStringLiteral("system")),
                                  makeSpec(QStringLiteral("microphone")) },
                                48000, workspace.path(), &error),
                 qPrintable(error));

        // Audio produced before the encoder accepts a frame is discarded.
        QTest::qWait(120);
        QCOMPARE(recorder.capturedMilliseconds(0), qint64(0));

        QElapsedTimer active;
        recorder.setCapturing(true);
        active.start();
        QTest::qWait(420);
        recorder.setCapturing(false);
        const qint64 firstSegmentMs = active.elapsed();
        const qint64 afterFirstSegment = recorder.capturedMilliseconds(0);
        QVERIFY2(afterFirstSegment > firstSegmentMs - 140 &&
                   afterFirstSegment < firstSegmentMs + 140,
                 qPrintable(QString("first segment %1 ms of %2 ms")
                              .arg(afterFirstSegment)
                              .arg(firstSegmentMs)));

        // The paused interval must not grow the recording, even though the
        // source keeps producing audio.
        QTest::qWait(280);
        QCOMPARE(recorder.capturedMilliseconds(0), afterFirstSegment);

        recorder.setCapturing(true);
        QElapsedTimer secondActive;
        secondActive.start();
        QTest::qWait(320);
        const qint64 secondSegmentMs = secondActive.elapsed();
        recorder.stop();

        const qint64 expectedMs = firstSegmentMs + secondSegmentMs;
        const qint64 systemMs = recorder.capturedMilliseconds(0);
        const qint64 micMs = recorder.capturedMilliseconds(1);
        QVERIFY2(qAbs(systemMs - expectedMs) < 180,
                 qPrintable(QString("system %1 ms of %2 ms active")
                              .arg(systemMs)
                              .arg(expectedMs)));
        QVERIFY2(qAbs(micMs - expectedMs) < 180,
                 qPrintable(QString("microphone %1 ms of %2 ms active")
                              .arg(micMs)
                              .arg(expectedMs)));
        QVERIFY(qAbs(systemMs - micMs) < 90);
        QCOMPARE(recorder.capturedBytes(1),
                 QFileInfo(recorder.filePath(1)).size());
        QVERIFY(QFileInfo(recorder.filePath(0)).size() > 0);

        const QString video = workspace.filePath(QStringLiteral("video.mp4"));
        bool ok = false;
        runTool(ffmpeg,
                { QStringLiteral("-hide_banner"), QStringLiteral("-nostdin"),
                  QStringLiteral("-loglevel"), QStringLiteral("error"),
                  QStringLiteral("-y"), QStringLiteral("-f"),
                  QStringLiteral("lavfi"), QStringLiteral("-i"),
                  QStringLiteral("testsrc=size=64x64:rate=10"),
                  QStringLiteral("-t"), QStringLiteral("1.0"),
                  QStringLiteral("-c:v"), QStringLiteral("libx264"),
                  QStringLiteral("-pix_fmt"), QStringLiteral("yuv420p"), video },
                &ok);
        QVERIFY2(ok, "synthetic video fixture encodes");
        if (!hasEncoder(ffmpeg, QStringLiteral("aac")))
            QSKIP("FFmpeg without an AAC encoder cannot mux MP4 audio.");

        // The video is longer than the audio: the audio must be padded to the
        // video length instead of truncating the recording.
        AudioMuxer::Request request;
        request.ffmpeg = ffmpeg;
        request.videoPath = video;
        request.container = QStringLiteral("mp4");
        request.codec = QStringLiteral("aac");
        request.bitrateKbps = 128;
        request.outputPath = workspace.filePath(QStringLiteral("mixed.mp4"));
        request.inputs = { { recorder.filePath(0), recorder.sampleRate(),
                             recorder.channels() },
                           { recorder.filePath(1), recorder.sampleRate(),
                             recorder.channels() } };
        QVERIFY2(AudioMuxer::validate(request, &error), qPrintable(error));
        AudioMuxer muxer;
        bool finished = false;
        QString failure;
        connect(&muxer, &AudioMuxer::succeeded, this,
                [&](const QString&) { finished = true; });
        connect(&muxer, &AudioMuxer::failed, this, [&](const QString& message) {
            failure = message;
            finished = true;
        });
        QVERIFY(muxer.start(request));
        QTRY_VERIFY_WITH_TIMEOUT(finished, 30000);
        QVERIFY2(failure.isEmpty(), qPrintable(failure));
        QVERIFY(QFileInfo(request.outputPath).size() > 0);

        if (ffprobe.isEmpty()) QSKIP("FFprobe is required to inspect the muxed file.");
        QCOMPARE(ffprobeField(ffprobe, request.outputPath,
                              QStringLiteral("stream=codec_name")),
                 QStringLiteral("aac"));
        QCOMPARE(ffprobeField(ffprobe, request.outputPath,
                              QStringLiteral("stream=channels")),
                 QStringLiteral("2"));
        QCOMPARE(ffprobeField(ffprobe, request.outputPath,
                              QStringLiteral("stream=sample_rate")),
                 QStringLiteral("48000"));
        bool durationOk = false;
        const double duration =
          formatDuration(ffprobe, request.outputPath).toDouble(&durationOk);
        QVERIFY2(durationOk, "muxed file reports a duration");
        QVERIFY2(duration > 0.85 && duration < 1.25,
                 qPrintable(QString("muxed duration %1 s").arg(duration)));
    }

    void muxesOpusIntoWebm()
    {
        const QString ffmpeg = ffmpegPath();
        if (ffmpeg.isEmpty()) QSKIP("FFmpeg is required for synthetic audio.");
        const QString ffprobe = ffprobePath();
        if (ffprobe.isEmpty()) QSKIP("FFprobe is required to inspect streams.");
        if (!hasEncoder(ffmpeg, QStringLiteral("libopus")) ||
            !hasEncoder(ffmpeg, QStringLiteral("libvpx-vp9")))
            QSKIP("FFmpeg without libopus/libvpx-vp9 cannot produce WebM audio.");
        QTemporaryDir workspace;
        QVERIFY(workspace.isValid());
        const QString video = workspace.filePath(QStringLiteral("video.webm"));
        bool ok = false;
        runTool(ffmpeg,
                { QStringLiteral("-hide_banner"), QStringLiteral("-nostdin"),
                  QStringLiteral("-loglevel"), QStringLiteral("error"),
                  QStringLiteral("-y"), QStringLiteral("-f"),
                  QStringLiteral("lavfi"), QStringLiteral("-i"),
                  QStringLiteral("testsrc=size=64x64:rate=5"), QStringLiteral("-t"),
                  QStringLiteral("0.6"), QStringLiteral("-c:v"),
                  QStringLiteral("libvpx-vp9"), QStringLiteral("-b:v"),
                  QStringLiteral("200k"), video },
                &ok);
        QVERIFY2(ok, "synthetic VP9 video fixture encodes");

        // Half a second of synthetic PCM, written straight to disk.
        const QString pcm = workspace.filePath(QStringLiteral("audio-0.pcm"));
        runTool(ffmpeg,
                { QStringLiteral("-hide_banner"), QStringLiteral("-nostdin"),
                  QStringLiteral("-loglevel"), QStringLiteral("error"),
                  QStringLiteral("-y"), QStringLiteral("-f"),
                  QStringLiteral("lavfi"), QStringLiteral("-i"),
                  QStringLiteral("sine=frequency=330:sample_rate=48000"),
                  QStringLiteral("-t"), QStringLiteral("0.5"),
                  QStringLiteral("-ac"), QStringLiteral("2"),
                  QStringLiteral("-f"), QStringLiteral("s16le"), pcm },
                &ok);
        QVERIFY2(ok, "synthetic PCM fixture renders");
        QVERIFY(QFileInfo(pcm).size() > 0);

        AudioMuxer::Request request;
        request.ffmpeg = ffmpeg;
        request.videoPath = video;
        request.container = QStringLiteral("webm");
        request.codec = Audio::audioCodecForContainer(QStringLiteral("webm"));
        request.bitrateKbps = 96;
        request.outputPath = workspace.filePath(QStringLiteral("mixed.webm"));
        request.inputs = { { pcm, 48000, 2 } };
        QString error;
        QVERIFY2(AudioMuxer::validate(request, &error), qPrintable(error));
        AudioMuxer muxer;
        bool finished = false;
        QString failure;
        connect(&muxer, &AudioMuxer::succeeded, this, [&](const QString&) { finished = true; });
        connect(&muxer, &AudioMuxer::failed, this, [&](const QString& message) {
            failure = message;
            finished = true;
        });
        QVERIFY(muxer.start(request));
        QTRY_VERIFY_WITH_TIMEOUT(finished, 30000);
        QVERIFY2(failure.isEmpty(), qPrintable(failure));
        QCOMPARE(ffprobeField(ffprobe, request.outputPath,
                              QStringLiteral("stream=codec_name")),
                 QStringLiteral("opus"));
    }

    void sessionMixesRequestedAudioAndRemovesPauses()
    {
        if (qEnvironmentVariableIsEmpty("DISPLAY")) {
            QSKIP("Recording needs an X display; run under Xephyr (see "
                  "AGENTS.md rule 1).");
        }
        const QString realFfmpeg = ffmpegPath();
        const QString ffprobe = ffprobePath();
        if (realFfmpeg.isEmpty() || ffprobe.isEmpty())
            QSKIP("FFmpeg and FFprobe are required for the audio session fixture.");
        if (!QFileInfo::exists(QStringLiteral("/usr/bin/python3")))
            QSKIP("Python is required for the fake FFmpeg fixture.");
        if (!hasEncoder(realFfmpeg, QStringLiteral("aac")) ||
            !hasEncoder(realFfmpeg, QStringLiteral("libx264")))
            QSKIP("FFmpeg without AAC or libx264 cannot run the audio session.");
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString binary = writeSessionFfmpegFixture(directory);
        QVERIFY(!binary.isEmpty());
        const QString destination = directory.filePath(QStringLiteral("audio.mp4"));

        bool saved = false;
        bool paused = false;
        QString warning;
        {
            CaptureRecordingSession session;
            connect(&session, &CaptureRecordingSession::saved, this,
                    [&](const QString& path, CaptureRecordingSession::Action) {
                saved = path == destination;
            });
            connect(&session, &CaptureRecordingSession::warning, this,
                    [&](const QString& message) { warning = message; });
            QVERIFY2(session.start(binary, QRect(0, 0, 2, 4), destination,
                                   audioOptionsForTest()),
                     qPrintable(session.lastError()));
            QVERIFY(session.hasAudio());
            QTRY_COMPARE_WITH_TIMEOUT(session.state(),
                                      X11RecordingBackend::State::Recording,
                                      5000);
            QTest::qWait(320);
            QVERIFY(session.togglePause());
            paused = true;
            QTest::qWait(240);
            QVERIFY(session.togglePause());
            QTest::qWait(260);
            session.finish();
            QTRY_VERIFY_WITH_TIMEOUT(saved, 40000);
        }
        QVERIFY(paused);
        QVERIFY2(warning.isEmpty(), qPrintable(warning));
        QVERIFY(QFileInfo(destination).size() > 0);
        // The temporary workspace, including every PCM file, is gone.
        QCOMPARE(QDir(directory.path())
                   .entryList({ ".flameshot-recording-*" },
                              QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot)
                   .size(),
                 0);
        QCOMPARE(ffprobeField(ffprobe, destination,
                              QStringLiteral("stream=codec_name")),
                 QStringLiteral("aac"));
        QCOMPARE(ffprobeField(ffprobe, destination,
                              QStringLiteral("stream=channels")),
                 QStringLiteral("2"));
        bool durationOk = false;
        const double duration =
          formatDuration(ffprobe, destination).toDouble(&durationOk);
        QVERIFY2(durationOk, "the muxed recording reports a duration");
        // Roughly 580 ms of active time; the 240 ms pause must be absent from
        // both the video and the audio timeline.
        QVERIFY2(duration > 0.35 && duration < 0.70,
                 qPrintable(QString("recording duration %1 s").arg(duration)));
        const QString audioDuration =
          runTool(ffprobe,
                  { QStringLiteral("-v"), QStringLiteral("error"),
                    QStringLiteral("-select_streams"), QStringLiteral("a:0"),
                    QStringLiteral("-show_entries"),
                    QStringLiteral("stream=duration"),
                    QStringLiteral("-of"),
                    QStringLiteral("default=nw=1:nk=1"), destination })
            .trimmed();
        const double audioSeconds = audioDuration.toDouble(&durationOk);
        QVERIFY2(durationOk && qAbs(audioSeconds - duration) < 0.2,
                 qPrintable(QString("audio %1 s vs container %2 s")
                              .arg(audioSeconds)
                              .arg(duration)));
    }

    void sessionCancelsAudioWithoutLeakingFiles()
    {
        const QString realFfmpeg = ffmpegPath();
        if (realFfmpeg.isEmpty()) QSKIP("FFmpeg is required for the session fixture.");
        if (!QFileInfo::exists(QStringLiteral("/usr/bin/python3")))
            QSKIP("Python is required for the fake FFmpeg fixture.");
        if (!hasEncoder(realFfmpeg, QStringLiteral("aac")) ||
            !hasEncoder(realFfmpeg, QStringLiteral("libx264")))
            QSKIP("FFmpeg without AAC or libx264 cannot run the audio session.");
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString binary = writeSessionFfmpegFixture(directory);
        const QString destination = directory.filePath(QStringLiteral("cancelled.mp4"));
        {
            CaptureRecordingSession session;
            QVERIFY2(session.start(binary, QRect(0, 0, 2, 4), destination,
                                   audioOptionsForTest()),
                     qPrintable(session.lastError()));
            QTRY_COMPARE_WITH_TIMEOUT(session.state(),
                                      X11RecordingBackend::State::Recording,
                                      5000);
            QTest::qWait(150);
            session.cancel();
        }
        QVERIFY(!QFileInfo::exists(destination));
        QCOMPARE(QDir(directory.path())
                   .entryList({ ".flameshot-recording-*" },
                              QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot)
                   .size(),
                 0);
    }

    void sessionRefusesMissingAudioService()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        // /bin/true reports no PulseAudio device and no encoders, so a request
        // for audio must fail with a specific message instead of saving a
        // silent video.
        const QString binary = QStringLiteral("/bin/true");
        CaptureRecordingSession session;
        RecordingOptions options = audioOptionsForTest();
        QVERIFY(!session.start(binary, QRect(0, 0, 2, 4),
                               directory.filePath(QStringLiteral("silent.mp4")),
                               options));
        QVERIFY2(!session.lastError().isEmpty(), "a clear startup error is set");
        QVERIFY(!QFileInfo::exists(directory.filePath(QStringLiteral("silent.mp4"))));
    }

    void recorderRejectsUnusableSetup()
    {
        QTemporaryDir workspace;
        QVERIFY(workspace.isValid());
        AudioRecorder recorder;
        QString error;
        QVERIFY(!recorder.start(QStringLiteral("/bin/true"), {}, 48000,
                                workspace.path(), &error));
        QVERIFY(!error.isEmpty());
        error.clear();
        QVERIFY(!recorder.start(
          QStringLiteral("/bin/true"),
          { { QStringLiteral("system"), QStringLiteral("lavfi"),
              QStringLiteral("sine"), true, {} } },
          100, workspace.path(), &error));
        QVERIFY(!error.isEmpty());
    }
};

QTEST_GUILESS_MAIN(AudioCaptureTest)
#include "audiocapturetest.moc"
