#include "ffmpegdiscovery.h"
#include "x11recordingbackend.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QTimer>

#include <cstdio>

using namespace Recording;

static bool require(bool value, const char *message)
{
    if (!value) std::fprintf(stderr, "FAIL: %s\n", message);
    return value;
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir temp;
    if (!require(temp.isValid(), "temporary directory available")) return 1;

    const QString fake = temp.filePath(QStringLiteral("ffmpeg"));
    QFile script(fake);
    if (!script.open(QIODevice::WriteOnly | QIODevice::Text)) return 1;
    script.write("#!/bin/sh\n"
                 "case \" $* \" in\n"
                 "  *' x11grab '*) while :; do printf '0123456789abcdefghijklmnopqrstuv'; /bin/sleep 0.02; done ;;\n"
                 "  *) for last do :; done; /bin/cat > \"$last\" ;;\n"
                 "esac\n");
    script.close();
    if (!QFile::setPermissions(fake, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                    QFileDevice::ExeOwner)) return 1;

    qputenv("PATH", temp.path().toLocal8Bit());
    const QString found = findFFmpeg({}, { temp.path() });
    if (!require(found == QFileInfo(fake).absoluteFilePath(), "common search path finds executable")) return 1;
    if (!require(findFFmpeg(temp.filePath(QStringLiteral("missing")), { temp.path() }).isEmpty(),
                 "invalid explicit path does not silently fall back")) return 1;

    const QString output = temp.filePath(QStringLiteral("recording.mkv"));
    X11RecordingBackend recorder;
    QElapsedTimer elapsed;
    elapsed.start();
    qint64 recordingStartedAt = -1;
    qint64 pauseStartedAt = -1;
    qint64 pausedDurationMs = 0;
    qint64 finalizingAt = -1;
    QObject::connect(&recorder, &X11RecordingBackend::stateChanged,
                     &app, [&](X11RecordingBackend::State state) {
        if (state == X11RecordingBackend::State::Recording) {
            if (recordingStartedAt < 0) recordingStartedAt = elapsed.elapsed();
            if (pauseStartedAt >= 0) {
                pausedDurationMs += elapsed.elapsed() - pauseStartedAt;
                pauseStartedAt = -1;
            }
        } else if (state == X11RecordingBackend::State::Paused) {
            pauseStartedAt = elapsed.elapsed();
        } else if (state == X11RecordingBackend::State::Finalizing) {
            finalizingAt = elapsed.elapsed();
        }
    });
    bool finished = false;
    bool failed = false;
    QObject::connect(&recorder, &X11RecordingBackend::recordingFinished,
                     &app, [&](const QString &) { finished = true; app.quit(); });
    QObject::connect(&recorder, &X11RecordingBackend::recordingFailed,
                     &app, [&](const QString &reason) {
                         std::fprintf(stderr, "recorder error: %s\n", qPrintable(reason));
                         failed = true;
                         app.quit();
                     });

    qputenv("DISPLAY", ":test");
    RecordingOptions options;
    options.frameRate = 60;
    options.codec = QStringLiteral("libx264");
    options.container = QStringLiteral("mkv");
    if (!require(!recorder.start(fake, QRect(0, 0, 2, 4),
                                 temp.filePath(QStringLiteral("wrong.mp4")), options),
                 "container must match output extension")) return 1;
    if (!require(recorder.start(fake, QRect(0, 0, 2, 4), output, options),
                 "fake capture and encoder start")) return 1;
    if (!require(recorder.state() == X11RecordingBackend::State::Preparing,
                 "start waits for a complete source frame before recording state")) return 1;
    QTimer::singleShot(150, &app, [&]() {
        if (!require(recorder.pause(), "pause succeeds while recording")) { app.quit(); return; }
        QTimer::singleShot(120, &app, [&]() {
            if (!require(recorder.resume(), "resume succeeds while paused")) { app.quit(); return; }
            QTimer::singleShot(150, &recorder, &X11RecordingBackend::stop);
        });
    });
    QTimer::singleShot(5000, &app, &QCoreApplication::quit);
    app.exec();
    if (!require(!failed && finished, "stop finalizes the encoder successfully")) return 1;
    QFile result(output);
    if (!require(result.exists() && result.size() > 0, "encoded output is present and nonempty")) return 1;
    // The fake capture emits 50 frames/s. Approximately 300 ms of active time
    // should survive; the 120 ms paused interval must not appear in the file.
    const qint64 frameBytes = 32;
    if (!require(result.size() % frameBytes == 0,
                 "encoder receives whole aligned frames")) return 1;
    const qint64 frameCount = result.size() / frameBytes;
    const qint64 activeMs = finalizingAt - recordingStartedAt - pausedDurationMs;
    const qint64 expectedFrames = activeMs * options.frameRate / 1000;
    if (!require(qAbs(frameCount - expectedFrames) <= 2,
                 "output frame count matches active recording duration")) return 1;
    if (!require(!recorder.pause(), "pause is rejected after finalization")) return 1;

    const QString externalOutput = temp.filePath(QStringLiteral("external.mkv"));
    X11RecordingBackend externalRecorder;
    bool externalFinished = false;
    bool externalFailed = false;
    QObject::connect(&externalRecorder, &X11RecordingBackend::recordingFinished,
                     &app, [&](const QString &) { externalFinished = true; app.quit(); });
    QObject::connect(&externalRecorder, &X11RecordingBackend::recordingFailed,
                     &app, [&](const QString &reason) {
                         std::fprintf(stderr, "external recorder error: %s\n", qPrintable(reason));
                         externalFailed = true;
                         app.quit();
                     });
    if (!require(externalRecorder.startEncoder(fake, QSize(2, 4), externalOutput, options),
                 "external frame encoder starts without X11 capture")) return 1;
    if (!require(externalRecorder.state() == X11RecordingBackend::State::Preparing,
                 "external encoder waits for its first frame")) return 1;
    QTimer sourceTimer;
    sourceTimer.setInterval(5);
    const QByteArray bgra(32, char(127));
    QObject::connect(&sourceTimer, &QTimer::timeout, &app, [&]() {
        externalRecorder.submitFrame(bgra, 2, 4, 8, elapsed.nsecsElapsed());
    });
    sourceTimer.start();
    QTimer::singleShot(260, &app, [&]() {
        sourceTimer.stop();
        externalRecorder.stop();
    });
    QTimer::singleShot(5000, &app, &QCoreApplication::quit);
    app.exec();
    if (!require(!externalFailed && externalFinished,
                 "external source finalizes successfully")) return 1;
    QFile externalResult(externalOutput);
    if (!require(externalResult.exists() && externalResult.size() % frameBytes == 0,
                 "external BGRA rows become complete encoder frames")) return 1;

    const QByteArray briefFrame(640 * 360 * 4, char(3));

    // A brief encoder stall is what a GUI stall produces on a healthy pipeline
    // (measured: a 150 ms stall queues about two frames and drains without
    // losing a frame). It is not a frame-rate problem and must not be reported
    // as one: the old rule warned on the first frame queued beyond one.
    {
        const QString stalled = temp.filePath(QStringLiteral("ffmpeg-brief-stall"));
        QFile stalledScript(stalled);
        if (!stalledScript.open(QIODevice::WriteOnly | QIODevice::Text)) return 1;
        stalledScript.write("#!/usr/bin/python3\n"
                            "import sys,time\n"
                            "size=640*360*4\n"
                            "count=0\n"
                            "with open(sys.argv[-1],'wb') as out:\n"
                            " while True:\n"
                            "  data=sys.stdin.buffer.read(size)\n"
                            "  if not data: break\n"
                            "  if len(data)!=size: sys.exit(2)\n"
                            "  count+=1\n"
                            "  if count==4: time.sleep(0.12)\n"
                            "  out.write(data)\n");
        stalledScript.close();
        if (!QFile::setPermissions(stalled, QFileDevice::ReadOwner |
                                               QFileDevice::WriteOwner |
                                               QFileDevice::ExeOwner)) return 1;
        if (!require(QFileInfo::exists(QStringLiteral("/usr/bin/python3")),
                     "python3 is available for the brief-stall fixture")) return 1;
        const QString briefOutput = temp.filePath(QStringLiteral("brief.mp4"));
        X11RecordingBackend briefRecorder;
        int briefWarnings = 0;
        bool briefFinished = false;
        bool briefFailed = false;
        QObject::connect(&briefRecorder, &X11RecordingBackend::recordingWarning,
                         &app, [&](const QString& message) {
                             ++briefWarnings;
                             std::fprintf(stderr, "unexpected warning: %s\n",
                                          qUtf8Printable(message));
                         });
        QObject::connect(&briefRecorder, &X11RecordingBackend::recordingFinished,
                         &app, [&](const QString&) {
                             briefFinished = true;
                             app.quit();
                         });
        QObject::connect(&briefRecorder, &X11RecordingBackend::recordingFailed,
                         &app, [&](const QString& reason) {
                             std::fprintf(stderr, "brief stall failed: %s\n",
                                          qUtf8Printable(reason));
                             briefFailed = true;
                             app.quit();
                         });
        if (!require(briefRecorder.startEncoder(stalled, QSize(640, 360), briefOutput),
                     "brief-stall encoder starts")) return 1;
        QTimer briefSource;
        briefSource.setInterval(33);
        QObject::connect(&briefSource, &QTimer::timeout, &app, [&]() {
            briefRecorder.submitFrame(briefFrame, 640, 360, 640 * 4, elapsed.nsecsElapsed());
        });
        briefSource.start();
        QTimer::singleShot(1200, &app, [&]() {
            briefSource.stop();
            briefRecorder.stop();
        });
        QTimer::singleShot(12000, &app, &QCoreApplication::quit);
        app.exec();
        if (!require(!briefFailed && briefFinished,
                     "a brief encoder stall still finalizes")) return 1;
        if (!require(briefWarnings == 0,
                     "a brief encoder stall is not reported as a frame-rate problem")) return 1;
        const QFileInfo briefResult(briefOutput);
        const qint64 briefFrames = 640 * 360 * 4 > 0 ? briefResult.size() / (640 * 360 * 4) : 0;
        if (!require(briefFrames >= 20,
                     "the brief stall still delivers the owed frames")) return 1;
    }

    // The opposite case has to keep its teeth: an encoder that stops consuming
    // for good is a real failure, reported as such rather than silently
    // shortening the recording.
    {
        const QString stuck = temp.filePath(QStringLiteral("ffmpeg-stuck"));
        QFile stuckScript(stuck);
        if (!stuckScript.open(QIODevice::WriteOnly | QIODevice::Text)) return 1;
        stuckScript.write("#!/usr/bin/python3\n"
                          "import sys,time\n"
                          "size=640*360*4\n"
                          "open(sys.argv[-1],'wb').close()\n"
                          "data=sys.stdin.buffer.read(size)\n"
                          "time.sleep(6)\n");
        stuckScript.close();
        if (!QFile::setPermissions(stuck, QFileDevice::ReadOwner |
                                             QFileDevice::WriteOwner |
                                             QFileDevice::ExeOwner)) return 1;
        const QString stuckOutput = temp.filePath(QStringLiteral("stuck.mp4"));
        X11RecordingBackend stuckRecorder;
        bool stuckFinished = false;
        bool stuckFailed = false;
        QString stuckReason;
        QObject::connect(&stuckRecorder, &X11RecordingBackend::recordingFinished,
                         &app, [&](const QString&) {
                             stuckFinished = true;
                             app.quit();
                         });
        QObject::connect(&stuckRecorder, &X11RecordingBackend::recordingFailed,
                         &app, [&](const QString& reason) {
                             stuckReason = reason;
                             stuckFailed = true;
                             app.quit();
                         });
        if (!require(stuckRecorder.startEncoder(stuck, QSize(640, 360), stuckOutput),
                     "stuck encoder starts")) return 1;
        QTimer stuckSource;
        stuckSource.setInterval(33);
        QObject::connect(&stuckSource, &QTimer::timeout, &app, [&]() {
            stuckRecorder.submitFrame(briefFrame, 640, 360, 640 * 4, elapsed.nsecsElapsed());
        });
        stuckSource.start();
        QTimer::singleShot(6000, &app, [&]() {
            // Nothing should still be recording here: the backlog failure has to
            // end the session well before this.
            stuckSource.stop();
            app.quit();
        });
        app.exec();
        stuckSource.stop();
        if (!require(stuckFailed && !stuckFinished,
                     "an encoder that stops consuming fails the recording")) return 1;
        if (!require(stuckReason.contains(QStringLiteral("keep up")),
                     "the failure names the frame-rate problem")) return 1;
        if (recorder.state() == X11RecordingBackend::State::Recording) return 1;
    }

    return 0;
}
