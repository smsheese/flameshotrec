// SPDX-License-Identifier: GPL-3.0-or-later
#include "capturerecordingsession.h"
#include <QtTest>
#include <QFile>
#include <QTemporaryDir>
#include <QStandardPaths>

using namespace Recording;
class CaptureRecordingSessionTest : public QObject
{
    Q_OBJECT
    static QString fakeFfmpeg(const QTemporaryDir& dir)
    {
        const QString path = dir.filePath("ffmpeg");
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly)) return {};
        file.write("#!/bin/sh\ncase \" $* \" in\n*' x11grab '*) while :; do printf '0123456789abcdefghijklmnopqrstuv'; /bin/sleep 0.02; done ;;\n*) for last do :; done; /bin/cat > \"$last\" ;;\nesac\n");
        file.close();
        file.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        return path;
    }
private slots:
    void intermittentEncoderStallsDoNotShortenVideo()
    {
        if (!QFileInfo::exists("/usr/bin/python3")) QSKIP("Python is required for the slow encoder fixture.");
        QTemporaryDir directory;
        const QString binary = directory.filePath("ffmpeg");
        QFile fixture(binary); QVERIFY(fixture.open(QIODevice::WriteOnly));
        fixture.write("#!/usr/bin/python3\nimport sys,time\nsize=640*360*4\ncount=0\nwith open(sys.argv[-1], 'wb') as out:\n while True:\n  data=sys.stdin.buffer.read(size)\n  if not data: break\n  if len(data)!=size: sys.exit(2)\n  count+=1\n  if count in (5,15,25,35): time.sleep(0.08)\n  out.write(b'x')\n");
        fixture.close(); fixture.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        X11RecordingBackend encoder;
        bool finished = false;
        QString error;
        QElapsedTimer clock;
        qint64 activeMs = 0;
        connect(&encoder, &X11RecordingBackend::stateChanged, this, [&](X11RecordingBackend::State state) {
            if (state == X11RecordingBackend::State::Recording) clock.start();
            if (state == X11RecordingBackend::State::Finalizing) activeMs = clock.elapsed();
        });
        connect(&encoder, &X11RecordingBackend::recordingFinished, this, [&]() { finished = true; });
        connect(&encoder, &X11RecordingBackend::recordingFailed, this, [&](const QString& message) { error = message; });
        const QString output = directory.filePath("timing.mp4");
        QVERIFY(encoder.startEncoder(binary, QSize(640, 360), output));
        const QByteArray frame(640 * 360 * 4, char(0));
        QTimer source;
        connect(&source, &QTimer::timeout, this, [&]() { encoder.submitFrame(frame, 640, 360, 640 * 4); });
        source.start(20);
        encoder.submitFrame(frame, 640, 360, 640 * 4);
        QTRY_COMPARE_WITH_TIMEOUT(encoder.state(), X11RecordingBackend::State::Recording, 2000);
        QTest::qWait(1400);
        source.stop(); encoder.stop();
        QTRY_VERIFY_WITH_TIMEOUT(finished || !error.isEmpty(), 5000);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        const qint64 frames = QFileInfo(output).size();
        const qint64 expected = activeMs * 30 / 1000;
        QVERIFY2(qAbs(frames - expected) <= 2, qPrintable(QString("%1 frames for %2 ms").arg(frames).arg(activeMs)));
    }

    void publishesOnlyAfterSave()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString binary = fakeFfmpeg(directory);
        const QString destination = directory.filePath("saved.mp4");
        bool saved = false;
        {
            CaptureRecordingSession session;
            connect(&session, &CaptureRecordingSession::saved, this,
                    [&](const QString& path, CaptureRecordingSession::Action) { saved = path == destination; });
            QVERIFY(session.start(binary, QRect(0, 0, 2, 4), destination, {}));
            QTRY_COMPARE_WITH_TIMEOUT(session.state(), X11RecordingBackend::State::Recording, 3000);
            QVERIFY(!QFileInfo::exists(destination));
            QTest::qWait(100);
            session.finish();
            QTRY_VERIFY_WITH_TIMEOUT(saved, 3000);
            QVERIFY(QFileInfo(destination).size() > 0);
        }
        QCOMPARE(QDir(directory.path()).entryList({".flameshot-recording-*"}, QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot).size(), 0);
    }

    void cancelDiscardsOnlyOwnedTemporaryFiles()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString binary = fakeFfmpeg(directory);
        const QString destination = directory.filePath("cancelled.mp4");
        QFile existing(directory.filePath("keep.mp4"));
        QVERIFY(existing.open(QIODevice::WriteOnly));
        existing.write("existing recording"); existing.close();
        {
            CaptureRecordingSession session;
            QVERIFY(session.start(binary, QRect(0, 0, 2, 4), destination, {}));
            QTRY_COMPARE_WITH_TIMEOUT(session.state(), X11RecordingBackend::State::Recording, 3000);
            session.cancel();
        }
        QVERIFY(!QFileInfo::exists(destination));
        QCOMPARE(QFileInfo(existing).size(), qint64(18));
        QCOMPARE(QDir(directory.path()).entryList({".flameshot-recording-*"}, QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot).size(), 0);
    }

    // Publishing is an atomic no-replace step, not an existence check made
    // earlier: a file that already holds the requested name is never touched, not
    // even when it appears while the recording is still running. The recording is
    // published under the next free name instead.
    void publishesBesideAnExistingFileWithoutReplacingIt()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString binary = fakeFfmpeg(directory);
        const QString destination = directory.filePath("saved.mp4");
        QFile existing(destination);
        QVERIFY(existing.open(QIODevice::WriteOnly));
        existing.write("a recording that must survive");
        existing.close();
        QString published;
        {
            CaptureRecordingSession session;
            connect(&session, &CaptureRecordingSession::saved, this,
                    [&](const QString& path, CaptureRecordingSession::Action) { published = path; });
            // A taken name is not a reason to refuse the recording: the configured
            // pattern repeats, so the second recording after the first save would
            // otherwise never start (and could not even be copied).
            QVERIFY(session.start(binary, QRect(0, 0, 2, 4), destination, {}));
            QTRY_COMPARE_WITH_TIMEOUT(session.state(), X11RecordingBackend::State::Recording, 3000);
            QTest::qWait(100);
            session.finish();
            QTRY_VERIFY_WITH_TIMEOUT(!published.isEmpty(), 3000);
        }
        QCOMPARE(published, directory.filePath("saved_1.mp4"));
        QVERIFY(QFileInfo(published).size() > 0);
        QVERIFY(existing.open(QIODevice::ReadOnly));
        QCOMPARE(existing.readAll(), QByteArray("a recording that must survive"));
        existing.close();
        // Neither the copy temporary nor a directory-scan guess survives.
        QCOMPARE(QDir(directory.path()).entryList({ "*.part-*", ".*.part-*" },
                                                  QDir::Files | QDir::Hidden),
                 QStringList());
    }

    void neverReplacesAFileThatAppearsWhileRecording()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString binary = fakeFfmpeg(directory);
        const QString destination = directory.filePath("race.mp4");
        QString published;
        {
            CaptureRecordingSession session;
            connect(&session, &CaptureRecordingSession::saved, this,
                    [&](const QString& path, CaptureRecordingSession::Action) { published = path; });
            // The name is free when the recording starts...
            QVERIFY(!QFileInfo::exists(destination));
            QVERIFY(session.start(binary, QRect(0, 0, 2, 4), destination, {}));
            QTRY_COMPARE_WITH_TIMEOUT(session.state(), X11RecordingBackend::State::Recording, 3000);
            // ...and taken by another writer before the publish.
            QFile appeared(destination);
            QVERIFY(appeared.open(QIODevice::WriteOnly));
            appeared.write("written by something else");
            appeared.close();
            QTest::qWait(100);
            session.finish();
            QTRY_VERIFY_WITH_TIMEOUT(!published.isEmpty(), 3000);
        }
        QCOMPARE(published, directory.filePath("race_1.mp4"));
        QVERIFY(QFileInfo(published).size() > 0);
        QFile appeared(destination);
        QVERIFY(appeared.open(QIODevice::ReadOnly));
        QCOMPARE(appeared.readAll(), QByteArray("written by something else"));
    }

    // A copy publishes under the exact name the clipboard offers, and a name that
    // is already taken is reported instead of replaced -- the finished recording
    // stays available, so the user can still save it.
    void copyPublishNeverReplacesAStagedFile()
    {
        QTemporaryDir directory;
        QTemporaryDir staging;
        QVERIFY(directory.isValid() && staging.isValid());
        const QString binary = fakeFfmpeg(directory);
        const QString destination = directory.filePath("saved.mp4");
        const QString staged = staging.filePath("copied.mp4");
        QString published;
        {
            CaptureRecordingSession session;
            connect(&session, &CaptureRecordingSession::saved, this,
                    [&](const QString& path, CaptureRecordingSession::Action) { published = path; });
            QVERIFY(session.start(binary, QRect(0, 0, 2, 4), destination, staged, {}));
            QTRY_COMPARE_WITH_TIMEOUT(session.state(), X11RecordingBackend::State::Recording, 3000);
            QTest::qWait(100);
            session.finish(CaptureRecordingSession::Action::Copy);
            QTRY_VERIFY_WITH_TIMEOUT(!published.isEmpty(), 3000);
        }
        QCOMPARE(published, staged);
        QVERIFY(QFileInfo(staged).size() > 0);
        // A copy never creates or writes the configured video folder.
        QVERIFY(!QFileInfo::exists(destination));
        QCOMPARE(QDir(staging.path()).entryList({ "*.part-*", ".*.part-*" },
                                                QDir::Files | QDir::Hidden),
                 QStringList());

        // A file an earlier paste is still reading must survive a second copy
        // that would take its name.
        QByteArray marker("still being read by a paste target");
        QFile blocker(staged);
        QVERIFY(blocker.open(QIODevice::WriteOnly));
        blocker.write(marker);
        blocker.close();
        CaptureRecordingSession second;
        QString failure;
        connect(&second, &CaptureRecordingSession::failed, this,
                [&](const QString& message) { failure = message; });
        QVERIFY(second.start(binary, QRect(0, 0, 2, 4), destination, staged, {}));
        QTRY_COMPARE_WITH_TIMEOUT(second.state(), X11RecordingBackend::State::Recording, 3000);
        QTest::qWait(100);
        second.finish(CaptureRecordingSession::Action::Copy);
        QTRY_VERIFY_WITH_TIMEOUT(!failure.isEmpty(), 3000);
        // The failure names the cause, not just a path.
        QVERIFY2(failure.contains("copied.mp4"), qPrintable(failure));
        QVERIFY(blocker.open(QIODevice::ReadOnly));
        QCOMPARE(blocker.readAll(), marker);
        // The finished recording is kept, so nothing the user recorded is lost.
        const auto recovery = QDir(staging.path())
                                .entryList({ ".flameshot-recording-*" },
                                           QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot);
        QCOMPARE(recovery.size(), 1);
        QVERIFY(QFileInfo(QDir(staging.path()).filePath(recovery.first() + "/capture.mp4")).size() > 0);
    }

    void preservesPartialOutputOnEncoderFailure()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString binary = directory.filePath("ffmpeg");
        QFile file(binary); QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("#!/bin/sh\ncase \" $* \" in\n*' x11grab '*) while :; do printf '0123456789abcdefghijklmnopqrstuv'; /bin/sleep 0.02; done ;;\n*) for last do :; done; printf 'partial recording' > \"$last\"; /bin/sleep 0.1; exit 1 ;;\nesac\n");
        file.close(); file.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        const QString destination = directory.filePath("failed.mp4");
        bool failed = false;
        {
            CaptureRecordingSession session;
            connect(&session, &CaptureRecordingSession::failed, this, [&](const QString&) { failed = true; });
            QVERIFY(session.start(binary, QRect(0, 0, 2, 4), destination, {}));
            QTRY_VERIFY_WITH_TIMEOUT(failed, 3000);
        }
        QVERIFY(!QFileInfo::exists(destination));
        const auto recovery = QDir(directory.path()).entryList({".flameshot-recording-*"}, QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot);
        QCOMPARE(recovery.size(), 1);
        QFile partial(QDir(directory.path()).filePath(recovery.first() + "/capture.mp4"));
        QVERIFY(partial.open(QIODevice::ReadOnly));
        QCOMPARE(partial.readAll(), QByteArray("partial recording"));
    }
};
QTEST_GUILESS_MAIN(CaptureRecordingSessionTest)
#include "capturerecordingsessiontest.moc"
