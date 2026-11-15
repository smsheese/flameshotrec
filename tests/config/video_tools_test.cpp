// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Flameshot Contributors

// Integrated check of the Video tab's "Annotation tools" section.
//
// The tab is built against a throw-away configuration directory, on a nested X
// server, so the real configuration, the real desktop and the real sound server
// are never touched (AGENTS.md rule 1).
//
// What it pins:
//   * opening (and re-showing) the Video tab never puts the configuration into
//     its error state and never writes a key the configuration does not know;
//   * the video/audio/encoder controls are still there next to the new section;
//   * the per-tool hold/fade/size values reach the configuration file and come
//     back through a fresh VideoConf;
//   * while the annotation keys are not registered in ConfigHandler the section
//     stays inactive and writes nothing, instead of bypassing the guard.

#include "config/videoconf.h"
#include "recording/annotationpolicy.h"
#include "recording/annotationpolicy_confighandler.h"
#include "utils/confighandler.h"

#include <QComboBox>
#include <QCheckBox>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QSet>
#include <QSettings>
#include <QSpinBox>
#include <QTest>

using Recording::AnnotationOption;
using Recording::AnnotationPolicy;
using Recording::AnnotationTool;

namespace {

bool annotationKeysRegistered()
{
    return ConfigHandler::recognizedGeneralOptions().contains(
      AnnotationPolicy::key(AnnotationTool::Pen, AnnotationOption::HoldMs));
}

QStringList unknownConfigKeys()
{
    // Keys present in the file that ConfigHandler would reject, i.e. what turns
    // the configuration into its error state on the next open.
    const QSet<QString> recognised = ConfigHandler::recognizedGeneralOptions();
    const QSet<QString> shortcuts = ConfigHandler::recognizedShortcutNames();
    const QString shortcutGroup = QStringLiteral("Shortcuts/");
    QStringList offenders;
    QSettings settings(ConfigHandler::getInstance()->configFilePath(),
                       QSettings::IniFormat);
    for (const QString& key : settings.allKeys()) {
        if (key.startsWith(shortcutGroup)) {
            if (!shortcuts.contains(key.mid(int(shortcutGroup.size())))) {
                offenders.append(key);
            }
            continue;
        }
        if (!recognised.contains(key)) {
            offenders.append(key);
        }
    }
    return offenders;
}

} // namespace

class VideoToolsTest : public QObject
{
    Q_OBJECT

private slots:
    void configurationStartsHealthy();
    void videoTabKeepsTheConfigurationHealthy();
    void videoSettingsAreStillPresent();
    void annotationSectionCoversEveryTool();
    void unregisteredKeysKeepTheSectionInactive();
    void perToolSettingsPersistThroughTheUi();
    void transferSettingsAreNoLongerOnTheVideoTab();
    void annotationSizeZeroIsRejectedByConfigAndUi();
};

void VideoToolsTest::configurationStartsHealthy()
{
    ConfigHandler* config = ConfigHandler::getInstance();
    QVERIFY2(config->checkForErrors(nullptr),
             qPrintable(unknownConfigKeys().join(QStringLiteral(", "))));
    QVERIFY(!config->hasError());
}

void VideoToolsTest::videoTabKeepsTheConfigurationHealthy()
{
    const QStringList before = unknownConfigKeys();
    QVERIFY2(before.isEmpty(), qPrintable(before.join(QStringLiteral(", "))));

    VideoConf video;
    video.updateComponents(); // the config window rebuilds the tab on changes
    video.updateComponents();

    ConfigHandler* config = ConfigHandler::getInstance();
    QVERIFY2(!config->hasError(),
             "opening the Video tab must not put the configuration in its error "
             "state");
    QVERIFY2(config->checkForErrors(nullptr),
             qPrintable(unknownConfigKeys().join(QStringLiteral(", "))));
    const QStringList after = unknownConfigKeys();
    QVERIFY2(after.isEmpty(), qPrintable(after.join(QStringLiteral(", "))));
}

void VideoToolsTest::videoSettingsAreStillPresent()
{
    // The tools section was added at the bottom; the controls a plain recording
    // needs must survive it.
    VideoConf video;
    const QStringList expected{ "ffmpegPath",    "videoPath",
                                "videoFilenamePattern", "frameRate",
                                "container",     "codec",
                                "rateControl",   "bitrate",
                                "quality",       "globalHoldDuration",
                                "globalFadeDuration", "audioSystem",
                                "audioMic",      "audioQuality",
                                "audioBitrate" };
    for (const QString& name : expected) {
        QVERIFY2(video.findChild<QWidget*>(name) != nullptr,
                 qPrintable(QStringLiteral("missing control: ") + name));
    }
}

void VideoToolsTest::annotationSectionCoversEveryTool()
{
    VideoConf video;
    auto* group = video.findChild<QGroupBox*>(QStringLiteral("annotationToolsGroup"));
    QVERIFY(group != nullptr);

    for (const auto tool : AnnotationPolicy::supportedTools()) {
        const QString identifier = AnnotationPolicy::toolIdentifier(tool);
        const QString holdName = QStringLiteral("toolHold_") + identifier;
        const QString fadeName = QStringLiteral("toolFade_") + identifier;
        QVERIFY2(video.findChild<QSpinBox*>(holdName) != nullptr,
                 qPrintable(QStringLiteral("no hold spinner for ") + identifier));
        QVERIFY2(video.findChild<QSpinBox*>(fadeName) != nullptr,
                 qPrintable(QStringLiteral("no fade spinner for ") + identifier));

        // A size control exists exactly when the tool really has a size axis.
        const QString sizeName = QStringLiteral("toolSize_") + identifier;
        const bool hasSize = AnnotationPolicy::sizeKind(tool) !=
                             Recording::AnnotationSizeKind::None;
        QVERIFY2((video.findChild<QSpinBox*>(sizeName) != nullptr) == hasSize,
                 qPrintable(QStringLiteral("size control mismatch for ") +
                            identifier));
    }

    // The two global values stay the single source of truth for "Default".
    auto* globalHold = video.findChild<QSpinBox*>(QStringLiteral("globalHoldDuration"));
    auto* penHold = video.findChild<QSpinBox*>(QStringLiteral("toolHold_pen"));
    QVERIFY(globalHold != nullptr);
    QVERIFY(penHold != nullptr);
    QCOMPARE(penHold->value(), Recording::kInherit);
    QCOMPARE(penHold->specialValueText(),
             QStringLiteral("Default (%1 ms)").arg(globalHold->value()));

    auto* counterMode = video.findChild<QComboBox*>(QStringLiteral("counterMode"));
    QVERIFY(counterMode != nullptr);
    QCOMPARE(counterMode->count(), 2);
    auto* hint = video.findChild<QLabel*>(QStringLiteral("counterModeHint"));
    QVERIFY(hint != nullptr);
    QVERIFY(!hint->text().isEmpty()); // the group timing rule is explained
}

void VideoToolsTest::unregisteredKeysKeepTheSectionInactive()
{
    VideoConf video;
    auto* group = video.findChild<QGroupBox*>(QStringLiteral("annotationToolsGroup"));
    auto* status = video.findChild<QLabel*>(QStringLiteral("annotationToolsStatus"));
    QVERIFY(group != nullptr);
    QVERIFY(status != nullptr);

    ConfigHandler* config = ConfigHandler::getInstance();

    if (annotationKeysRegistered()) {
        QVERIFY(group->isEnabled());
        QVERIFY(status->isHidden());
        QSKIP("annotation keys are registered: the pending-registration branch no "
              "longer applies");
    }

    // Not registered yet: the section must not write behind the guard.
    QVERIFY(!group->isEnabled());
    QVERIFY(!status->isHidden());
    QVERIFY(status->text().contains(AnnotationPolicy::key(AnnotationTool::Pen,
                                                          AnnotationOption::HoldMs)));

    const QStringList keys = unknownConfigKeys();
    QVERIFY2(keys.isEmpty(), qPrintable(keys.join(QStringLiteral(", "))));
    QVERIFY(!config->hasError());
}

void VideoToolsTest::perToolSettingsPersistThroughTheUi()
{
    if (!annotationKeysRegistered()) {
        QSKIP("annotation keys are not registered in ConfigHandler yet; see "
              "/tmp/flameshot-next-notes/tool-config-hooks.md");
    }

    // Write through the widgets, exactly as a user would.
    {
        VideoConf video;
        auto* penHold = video.findChild<QSpinBox*>(QStringLiteral("toolHold_pen"));
        auto* penFade = video.findChild<QSpinBox*>(QStringLiteral("toolFade_pen"));
        auto* pixelateSize =
          video.findChild<QSpinBox*>(QStringLiteral("toolSize_pixelate"));
        auto* counterMode = video.findChild<QComboBox*>(QStringLiteral("counterMode"));
        QVERIFY(penHold && penFade && pixelateSize && counterMode);

        penHold->setValue(1200);
        penFade->setValue(400);
        pixelateSize->setValue(12);
        counterMode->setCurrentIndex(1); // fade the group from its first bubble
    }

    // A fresh tab reads the values back from the throw-away configuration.
    VideoConf reopened;
    auto* penHold = reopened.findChild<QSpinBox*>(QStringLiteral("toolHold_pen"));
    auto* penFade = reopened.findChild<QSpinBox*>(QStringLiteral("toolFade_pen"));
    auto* pixelateSize =
      reopened.findChild<QSpinBox*>(QStringLiteral("toolSize_pixelate"));
    auto* counterMode = reopened.findChild<QComboBox*>(QStringLiteral("counterMode"));
    QCOMPARE(penHold->value(), 1200);
    QCOMPARE(penFade->value(), 400);
    QCOMPARE(pixelateSize->value(), 12);
    QCOMPARE(counterMode->currentIndex(), 1);

    // Per-tool independence: the other tools still inherit the global value.
    auto* globalHold =
      reopened.findChild<QSpinBox*>(QStringLiteral("globalHoldDuration"));
    auto* lineHold = reopened.findChild<QSpinBox*>(QStringLiteral("toolHold_line"));
    QCOMPARE(lineHold->value(), Recording::kInherit);
    QCOMPARE(lineHold->specialValueText(),
             QStringLiteral("Default (%1 ms)").arg(globalHold->value()));

    const AnnotationPolicy policy =
      Recording::loadAnnotationPolicyFromConfig();
    QCOMPARE(policy.timing(AnnotationTool::Pen).holdMs, 1200);
    QCOMPARE(policy.timing(AnnotationTool::Line).holdMs,
             globalHold->value());
    QCOMPARE(policy.counterMode(),
             Recording::AnnotationCounterMode::GroupFromFirstBubble);

    // And the file itself carries no key the configuration would reject.
    const QStringList keys = unknownConfigKeys();
    QVERIFY2(keys.isEmpty(), qPrintable(keys.join(QStringLiteral(", "))));
    QVERIFY(!ConfigHandler::getInstance()->hasError());
}

// Audit finding 7: the policy accepts only -1 (inherit) or 1..50, so the
// configuration guard must reject 0 as well, and the spin box must not leave a
// user sitting on a value the policy will refuse.
void VideoToolsTest::annotationSizeZeroIsRejectedByConfigAndUi()
{
    if (!annotationKeysRegistered()) {
        QSKIP("annotation keys are not registered in ConfigHandler yet");
    }

    // 1. The UI never keeps 0: selecting it snaps back to "inherit".
    {
        VideoConf video;
        auto* size = video.findChild<QSpinBox*>(QStringLiteral("toolSize_pen"));
        QVERIFY(size != nullptr);
        size->setValue(0);
        QCOMPARE(size->value(), Recording::kInherit);
        size->setValue(7);
        QCOMPARE(size->value(), 7);
    }

    // 2. A hand-edited 0 is a configuration error, not a silently accepted value.
    {
        QSettings settings(ConfigHandler::getInstance()->configFilePath(),
                           QSettings::IniFormat);
        settings.setValue(QStringLiteral("annotationPenSize"), 0);
        settings.sync();
    }
    ConfigHandler* config = ConfigHandler::getInstance();
    QVERIFY2(!config->checkForErrors(nullptr),
             "a size of 0 must be reported as a configuration error");
    // The reported fallback is "inherit", and the bad value is not handed on.
    QCOMPARE(config->value(QStringLiteral("annotationPenSize")).toInt(), -1);

    // Leave the throw-away configuration healthy again for any later case.
    {
        QSettings settings(config->configFilePath(), QSettings::IniFormat);
        settings.remove(QStringLiteral("annotationPenSize"));
        settings.sync();
    }
    config->checkForErrors(nullptr);
}

void VideoToolsTest::transferSettingsAreNoLongerOnTheVideoTab()
{
    // The upload settings moved to their own tab; the Video tab must not keep a
    // second copy of them.
    VideoConf video;
    const auto labels = video.findChildren<QLabel*>();
    for (const QLabel* label : labels) {
        QVERIFY2(!label->text().contains(QStringLiteral("Remote directory")),
                 "the transfer settings still live on the Video tab");
        QVERIFY2(!label->text().contains(QStringLiteral("Known hosts")),
                 "the transfer settings still live on the Video tab");
    }
}

QTEST_MAIN(VideoToolsTest)
#include "video_tools_test.moc"
