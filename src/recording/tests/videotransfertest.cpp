// SPDX-License-Identifier: GPL-3.0-or-later
#include "videotransfer.h"

#include <QCoreApplication>
#include <QSignalSpy>
#include <QTemporaryFile>
#include <QTemporaryDir>
#include <QDir>
#include <QScopeGuard>
#include <QtTest>

class VideoTransferTest : public QObject
{
    Q_OBJECT
private slots:
    void formatsIpv6SftpHostsOnce()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QByteArray oldPath = qgetenv("PATH");
        const QByteArray oldArgs = qgetenv("FLAMESHOT_SFTP_ARGS_FILE");
        const auto restore = qScopeGuard([&]() { qputenv("PATH", oldPath); qputenv("FLAMESHOT_SFTP_ARGS_FILE", oldArgs); });
        const QString arguments = directory.filePath("arguments");
        QFile binary(directory.filePath("sftp"));
        QVERIFY(binary.open(QIODevice::WriteOnly));
        // The uploader builds a multi-step sftp plan (upload to the per-transfer
        // temporary, read its size back with `ls -l`, then `rename`), so a shim
        // that only recorded argv could never satisfy the post-upload size check:
        // it has to answer the listing step for the path the batch names, which
        // is what a real server does. The upload is then accepted on the plan the
        // source actually builds, not on an older one.
        binary.write(R"SCRIPT(#!/bin/sh
# Record every argument, one per line, so the invocation can be inspected.
printf '%s\n' "$@" >> "$FLAMESHOT_SFTP_ARGS_FILE"
batch=$(/bin/cat)
local_path=$(printf '%s\n' "$batch" | /bin/sed -n 's/^put "\(.*\)" ".*"$/\1/p' | /usr/bin/head -n 1)
remote_path=$(printf '%s\n' "$batch" | /bin/sed -n 's/^ls -l "\(.*\)"$/\1/p' | /usr/bin/head -n 1)
if [ -n "$remote_path" ] && [ -f "$local_path" ]; then
    size=$(/usr/bin/wc -c < "$local_path" | /usr/bin/tr -d ' ')
    printf -- '-rw-r--r--    1 owner group %s Nov 15 10:00 %s\n' "$size" "$remote_path"
fi
exit 0
)SCRIPT");
        binary.close(); binary.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        // The shim has to win the lookup, but replacing PATH entirely also hides
        // the real helper binaries the uploader needs, so prepend it instead.
        qputenv("PATH",
                directory.path().toLocal8Bit() + ':' + qgetenv("PATH"));
        qputenv("FLAMESHOT_SFTP_ARGS_FILE", arguments.toLocal8Bit());
        QFile file(directory.filePath("video.mp4"));
        QVERIFY(file.open(QIODevice::WriteOnly)); file.write("fixture"); file.close();
        for (const QString& host : {QStringLiteral("::1"), QStringLiteral("[::1]")}) {
            // Per iteration: the recorded arguments describe this invocation only.
            QFile::remove(arguments);
            VideoTransfer transfer;
            VideoTransferOptions options;
            options.enabled = true; options.protocol = VideoTransferOptions::Protocol::SFTP;
            options.host = host; options.username = "fixture";
            // The facade validates its settings, so a complete set is required:
            // an SFTP destination needs a remote directory as well.
            options.remoteDirectory = QStringLiteral("/");
            options.privateKeyPath = file.fileName(); options.knownHostsPath = file.fileName();
            QSignalSpy succeeded(&transfer, &VideoTransfer::succeeded), failed(&transfer, &VideoTransfer::failed);
            const bool accepted = transfer.upload(file.fileName(), options);
            if (!accepted && !failed.isEmpty()) {
                // A refusal has to say why, otherwise a failing upload is
                // undiagnosable from the test output alone.
                qWarning("upload refused: %s",
                         qUtf8Printable(failed.first().at(0).toString()));
            }
            QVERIFY(accepted);
            QTRY_VERIFY_WITH_TIMEOUT(succeeded.count() || failed.count(), 3000);
            QVERIFY2(failed.empty(), failed.empty() ? "" : qPrintable(failed.first().at(0).toString()));
            QCOMPARE(succeeded.count(), 1);
            // The local recording is never removed by a transfer, whatever the
            // destination did.
            QVERIFY(QFileInfo::exists(file.fileName()));
            QFile args(arguments); QVERIFY(args.open(QIODevice::ReadOnly));
            const QStringList list = QString::fromUtf8(args.readAll()).split('\n');
            // Every assertion below reports the recorded invocation, so a broken
            // argument can be read without instrumenting the test again.
            const QString recorded = list.join(QLatin1Char(' '));
            auto fails = [&](const QString& what) {
                return qPrintable(QStringLiteral("%1 not honoured; recorded: %2")
                                    .arg(what, recorded));
            };
            // A literal IPv6 host is bracketed exactly once, whether the user
            // typed brackets or not: one host argument per invocation, never two
            // bracket pairs and never a bare literal.
            QVERIFY2(list.contains("fixture@[::1]"), fails("IPv6 host bracketing"));
            QVERIFY2(list.filter("[[::1]]").isEmpty(), fails("single bracket pair"));
            QVERIFY2(list.filter("fixture@::1").isEmpty(), fails("bracketed host"));
            // One host argument per invocation (`-b -` makes sftp read the batch
            // from stdin, one invocation per plan step: upload, then rename).
            int hostArguments = 0;
            int invocations = 0;
            for (const QString& token : list) {
                if (token == QLatin1String("fixture@[::1]")) {
                    ++hostArguments;
                } else if (token == QLatin1String("-b")) {
                    ++invocations;
                }
            }
            QVERIFY2(hostArguments >= 1 && hostArguments == invocations,
                     fails("one host argument per sftp invocation"));
            // The trust model is unchanged: the configured known_hosts file is
            // the only anchor, strict checking stays on, the global database is
            // disabled and no user client config is read.
            QVERIFY2(list.contains("-F"), fails("-F none"));
            QVERIFY2(list.contains("none"), fails("-F none"));
            QVERIFY2(list.contains("-oStrictHostKeyChecking=yes"),
                     fails("strict host key checking"));
            QVERIFY2(list.contains("-oGlobalKnownHostsFile=/dev/null"),
                     fails("disabled global known_hosts"));
            const QString knownHostsArgument =
              QStringLiteral("-oUserKnownHostsFile=\"%1\"").arg(file.fileName());
            QVERIFY2(list.contains(knownHostsArgument),
                     fails("the configured known_hosts file"));
            QVERIFY2(list.contains("-oBatchMode=yes"), fails("batch mode"));
            QVERIFY2(list.contains("-oIdentitiesOnly=yes"),
                     fails("identities only"));
            QVERIFY2(list.contains("-i"), fails("identity option"));
            QVERIFY2(list.contains(file.fileName()), fails("the private key path"));
        }
    }

    void uploadsToLocalSftpFixture()
    {
        const QString fixture = qEnvironmentVariable("FLAMESHOT_SFTP_FIXTURE");
        if (fixture.isEmpty()) QSKIP("Set FLAMESHOT_SFTP_FIXTURE for the isolated localhost SSH fixture.");
        QTemporaryDir files;
        QTemporaryDir remote(fixture + "/uploaded-XXXXXX");
        QVERIFY(files.isValid() && remote.isValid());
        const QString localPath = files.filePath("video file [literal].mp4");
        QFile file(localPath);
        QVERIFY(file.open(QIODevice::WriteOnly));
        const QByteArray payload(4096, char(0x6a)); file.write(payload); file.close();
        VideoTransferOptions options;
        options.enabled = true;
        options.protocol = VideoTransferOptions::Protocol::SFTP;
        options.host = "127.0.0.1"; options.port = 22221;
        options.username = qEnvironmentVariable("USER");
        options.privateKeyPath = fixture + "/client_key";
        options.knownHostsPath = fixture + "/known_hosts";
        options.remoteDirectory = remote.path();
        VideoTransfer transfer;
        QSignalSpy succeeded(&transfer, &VideoTransfer::succeeded);
        QSignalSpy failed(&transfer, &VideoTransfer::failed);
        QVERIFY(transfer.upload(localPath, options));
        QTRY_VERIFY_WITH_TIMEOUT(succeeded.count() || failed.count(), 10000);
        QVERIFY2(failed.isEmpty(), failed.isEmpty() ? "" : qPrintable(failed.first().first().toString()));
        QCOMPARE(succeeded.count(), 1);
        QVERIFY(QFileInfo::exists(localPath));
        // The published object carries a per-transfer unique name (that suffix is
        // the SFTP no-clobber guarantee: two uploads of the same base name can
        // never replace each other). Assert what the contract promises -- one
        // object in the target directory, the local base name as prefix, the same
        // bytes -- instead of the bare local file name, which the uploader has not
        // used since the unique-name design landed.
        const QStringList remoteFiles =
          QDir(remote.path()).entryList(QDir::Files, QDir::Name);
        QCOMPARE(remoteFiles.size(), 1);
        const QString uploadedName = remoteFiles.value(0);
        QVERIFY2(uploadedName.startsWith(QFileInfo(localPath).completeBaseName()),
                 qPrintable(uploadedName));
        QVERIFY2(uploadedName.endsWith(QLatin1String(".mp4")), qPrintable(uploadedName));
        QFile uploaded(remote.filePath(uploadedName));
        QVERIFY(uploaded.open(QIODevice::ReadOnly)); QCOMPARE(uploaded.readAll(), payload);
    }

    void refusesUnknownSftpHost()
    {
        const QString fixture = qEnvironmentVariable("FLAMESHOT_SFTP_FIXTURE");
        if (fixture.isEmpty()) QSKIP("Local SFTP fixture not enabled.");
        QTemporaryFile local; QVERIFY(local.open()); local.write("keep local recording"); local.flush();
        QTemporaryFile unknownHosts; QVERIFY(unknownHosts.open()); unknownHosts.flush();
        VideoTransferOptions options;
        options.enabled = true; options.protocol = VideoTransferOptions::Protocol::SFTP;
        options.host = "127.0.0.1"; options.port = 22221;
        options.username = qEnvironmentVariable("USER");
        options.privateKeyPath = fixture + "/client_key";
        options.knownHostsPath = unknownHosts.fileName();
        VideoTransfer transfer;
        QSignalSpy succeeded(&transfer, &VideoTransfer::succeeded);
        QSignalSpy failed(&transfer, &VideoTransfer::failed);
        QVERIFY(transfer.upload(local.fileName(), options));
        QTRY_VERIFY_WITH_TIMEOUT(!failed.empty(), 10000);
        QVERIFY(succeeded.empty()); QVERIFY(QFileInfo::exists(local.fileName()));
    }

    void uploadsToLocalFtpFixture()
    {
        const QString fixture = qEnvironmentVariable("FLAMESHOT_FTP_FIXTURE");
        if (fixture.isEmpty()) QSKIP("Set FLAMESHOT_FTP_FIXTURE for the isolated localhost FTP fixture.");
        QTemporaryDir files; QVERIFY(files.isValid());
        const QString path = files.filePath("ftp video.mp4");
        QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly));
        const QByteArray payload(4096, char(0x3c)); file.write(payload); file.close();
        VideoTransferOptions options;
        options.enabled = true; options.protocol = VideoTransferOptions::Protocol::FTP;
        options.host = "127.0.0.1"; options.port = 21221;
        options.username = "fixture"; options.password = "local-fixture-only";
        options.remoteDirectory = "/";
        VideoTransfer transfer;
        QSignalSpy succeeded(&transfer, &VideoTransfer::succeeded), failed(&transfer, &VideoTransfer::failed);
        QVERIFY(transfer.upload(path, options));
        QTRY_VERIFY_WITH_TIMEOUT(succeeded.count() || failed.count(), 10000);
        QVERIFY2(failed.empty(), failed.empty() ? "" : qPrintable(failed.first().first().toString()));
        QCOMPARE(succeeded.count(), 1); QVERIFY(QFileInfo::exists(path));
        // Same unique-name rule as SFTP: assert one object with the local base
        // name as prefix and the exact bytes, not the bare file name.
        const QStringList uploadedNames =
          QDir(fixture).entryList(QDir::Files, QDir::Name);
        QCOMPARE(uploadedNames.size(), 1);
        QFile uploaded(QDir(fixture).filePath(uploadedNames.value(0)));
        QVERIFY(uploaded.open(QIODevice::ReadOnly)); QCOMPARE(uploaded.readAll(), payload);
    }

    void refusesMissingLocalFile()
    {
        VideoTransfer transfer;
        QSignalSpy failed(&transfer, &VideoTransfer::failed);
        VideoTransferOptions options;
        options.enabled = true;
        options.host = "example.invalid";
        QVERIFY(!transfer.upload("/definitely/not/a/recording.mp4", options));
        QCOMPARE(failed.count(), 1);
        QVERIFY(failed.takeFirst().at(0).toString().contains("missing or unreadable"));
    }

    void validatesSftpHostKeyInputs()
    {
        QTemporaryFile file;
        QVERIFY(file.open());
        file.write("video");
        file.flush();
        VideoTransfer transfer;
        QSignalSpy failed(&transfer, &VideoTransfer::failed);
        VideoTransferOptions options;
        options.enabled = true;
        options.host = "server.example";
        options.protocol = VideoTransferOptions::Protocol::SFTP;
        options.username = "user";
        options.privateKeyPath = file.fileName();
        QVERIFY(!transfer.upload(file.fileName(), options));
        QCOMPARE(failed.count(), 1);
        QVERIFY(failed.takeFirst().at(0).toString().contains("known_hosts"));
    }
};

QTEST_GUILESS_MAIN(VideoTransferTest)
#include "videotransfertest.moc"
