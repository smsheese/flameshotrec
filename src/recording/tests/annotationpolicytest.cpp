// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Flameshot Contributors

// Focused regression tests for the per-tool annotation policy: the on-disk key
// format, default inheritance from the recording-wide hold/fade, per-tool
// independence, validation of stored and requested values, and the counter
// group rule. The configuration layer is replaced by an in-memory store, so no
// real configuration file, display server or recording is involved.

#include "recording/annotationpolicy.h"

#include <QHash>
#include <QTest>

using Recording::AnnotationCounterMode;
using Recording::AnnotationOption;
using Recording::AnnotationPolicy;
using Recording::AnnotationStore;
using Recording::AnnotationTiming;
using Recording::AnnotationTool;

namespace {

// In-memory configuration: the policy never talks to ConfigHandler directly, so
// the tests exercise exactly the code the application runs.
class MapStore : public AnnotationStore
{
public:
    bool isAvailable(const QString& key) const override
    {
        return !m_unavailable.contains(key);
    }
    QVariant read(const QString& key) const override
    {
        return m_values.value(key);
    }
    bool write(const QString& key, const QVariant& value) override
    {
        m_writes.append(key);
        m_values.insert(key, value);
        return true;
    }
    void remove(const QString& key) override
    {
        m_values.remove(key);
        m_writes.append(key);
    }

    void setUnavailable(const QStringList& keys)
    {
        m_unavailable = keys;
    }
    // Bypasses write(): simulates a hand-edited file that the configuration
    // layer did not range-check.
    void forceValue(const QString& key, const QVariant& value)
    {
        m_values.insert(key, value);
    }
    QStringList writes() const { return m_writes; }
    int writeCount() const { return int(m_writes.size()); }

private:
    QHash<QString, QVariant> m_values;
    QStringList m_unavailable;
    QStringList m_writes;
};

} // namespace

class AnnotationPolicyTest : public QObject
{
    Q_OBJECT

private slots:
    void supportedToolsAreStable();
    void keysFollowTheAnnotationPrefix();
    void defaultsInheritTheGlobalTiming();
    void unsetToolsFollowChangedGlobals();
    void persistenceRoundTrips();
    void perToolIndependence();
    void invalidValuesAreRejectedAndNotWritten();
    void outOfRangeStoredValuesFallBackToInherit();
    void counterModeRoundTripsAndValidates();
    void counterEffectiveTiming();
    void counterGroupStamp();
    void unregisteredKeysAreReportedAndRefused();
    void resetToolRestoresInheritance();
    void sizeResolutionUsesTheDrawingDefault();
};

// The identifiers and key names are written to the user's configuration file.
// Renaming one silently resets that tool for every existing user, so the exact
// list is pinned here.
void AnnotationPolicyTest::supportedToolsAreStable()
{
    const QList<AnnotationTool> tools = AnnotationPolicy::supportedTools();
    QStringList identifiers;
    for (const auto tool : tools) {
        identifiers.append(AnnotationPolicy::toolIdentifier(tool));
    }
    QCOMPARE(identifiers,
             QStringList({ "pen", "line", "arrow", "box", "circle", "marker",
                           "text", "selection", "counter", "invert",
                           "pixelate" }));

    QStringList keys = AnnotationPolicy::configuredKeys();
    QCOMPARE(keys.first(), QStringLiteral("annotationPenHoldMs"));
    QCOMPARE(keys.last(), QStringLiteral("annotationCounterMode"));
    // 11 tools x (hold + fade) + 10 sizes (every tool except Invert has one)
    // + the counter mode.
    QCOMPARE(int(keys.size()), 11 * 2 + 10 + 1);
    QCOMPARE(QSet<QString>(keys.begin(), keys.end()).size(), int(keys.size()));
}

void AnnotationPolicyTest::keysFollowTheAnnotationPrefix()
{
    QCOMPARE(AnnotationPolicy::key(AnnotationTool::Pen, AnnotationOption::HoldMs),
             QStringLiteral("annotationPenHoldMs"));
    QCOMPARE(AnnotationPolicy::key(AnnotationTool::Pen, AnnotationOption::FadeMs),
             QStringLiteral("annotationPenFadeMs"));
    QCOMPARE(AnnotationPolicy::key(AnnotationTool::Pen, AnnotationOption::Size),
             QStringLiteral("annotationPenSize"));
    QCOMPARE(AnnotationPolicy::key(AnnotationTool::Counter,
                                   AnnotationOption::Size),
             QStringLiteral("annotationCounterSize"));
    // Invert has no size axis, so it has no size key at all: the settings
    // section must not offer a control for it.
    QVERIFY(AnnotationPolicy::key(AnnotationTool::Invert,
                                  AnnotationOption::Size)
              .isEmpty());
    QCOMPARE(AnnotationPolicy::sizeKind(AnnotationTool::Invert),
             Recording::AnnotationSizeKind::None);
    QCOMPARE(AnnotationPolicy::counterModeKey(),
             QStringLiteral("annotationCounterMode"));
}

void AnnotationPolicyTest::defaultsInheritTheGlobalTiming()
{
    const AnnotationPolicy policy = AnnotationPolicy::defaults(5000, 700);
    for (const auto tool : AnnotationPolicy::supportedTools()) {
        const auto& settings = policy.settings(tool);
        QCOMPARE(settings.configured.holdMs, Recording::kInherit);
        QCOMPARE(settings.configured.fadeMs, Recording::kInherit);
        QCOMPARE(settings.configuredSize, Recording::kInherit);
        QCOMPARE(policy.timing(tool).holdMs, 5000);
        QCOMPARE(policy.timing(tool).fadeMs, 700);
    }
    QCOMPARE(policy.counterMode(),
             Recording::AnnotationCounterMode::HoldUntilToolChange);
    QVERIFY(policy.isFullyAvailable()); // no store was consulted
}

void AnnotationPolicyTest::unsetToolsFollowChangedGlobals()
{
    MapStore store;
    const AnnotationPolicy slow = AnnotationPolicy::load(store, 8000, 900);
    QCOMPARE(slow.timing(AnnotationTool::Pen).holdMs, 8000);
    QCOMPARE(slow.timing(AnnotationTool::Pen).fadeMs, 900);

    // A user who raised the global values without ever touching this section
    // gets the new values everywhere: nothing was frozen into per-tool entries.
    const AnnotationPolicy faster = AnnotationPolicy::load(store, 2000, 100);
    QCOMPARE(faster.timing(AnnotationTool::Pen).holdMs, 2000);
    QCOMPARE(faster.timing(AnnotationTool::Marker).fadeMs, 100);
    QVERIFY(store.writes().isEmpty());
}

void AnnotationPolicyTest::persistenceRoundTrips()
{
    MapStore store;
    QVERIFY(AnnotationPolicy::setTiming(store, AnnotationTool::Pen, { 1200, 400 }));
    QVERIFY(AnnotationPolicy::setHoldMs(store, AnnotationTool::Line, 2500));
    QVERIFY(AnnotationPolicy::setFadeMs(store, AnnotationTool::Text, 0));
    QVERIFY(AnnotationPolicy::setSize(store, AnnotationTool::Pixelate, 12));
    QVERIFY(AnnotationPolicy::setCounterMode(
      store, AnnotationCounterMode::GroupFromFirstBubble));

    const AnnotationPolicy loaded = AnnotationPolicy::load(store, 3000, 300);
    QCOMPARE(loaded.settings(AnnotationTool::Pen).configured.holdMs, 1200);
    QCOMPARE(loaded.settings(AnnotationTool::Pen).configured.fadeMs, 400);
    QCOMPARE(loaded.timing(AnnotationTool::Pen).holdMs, 1200);
    QCOMPARE(loaded.timing(AnnotationTool::Pen).fadeMs, 400);
    // A single duration set on its own leaves the other one inheriting.
    QCOMPARE(loaded.settings(AnnotationTool::Line).configured.holdMs, 2500);
    QCOMPARE(loaded.settings(AnnotationTool::Line).configured.fadeMs,
             Recording::kInherit);
    QCOMPARE(loaded.timing(AnnotationTool::Line).fadeMs, 300);
    QCOMPARE(loaded.timing(AnnotationTool::Text).fadeMs, 0);
    QCOMPARE(loaded.configuredSize(AnnotationTool::Pixelate), 12);
    QCOMPARE(loaded.counterMode(),
             AnnotationCounterMode::GroupFromFirstBubble);
    QVERIFY(loaded.isFullyAvailable());
}

void AnnotationPolicyTest::perToolIndependence()
{
    MapStore store;
    QVERIFY(AnnotationPolicy::setHoldMs(store, AnnotationTool::Pen, 1500));
    QVERIFY(AnnotationPolicy::setSize(store, AnnotationTool::Box, 9));

    const AnnotationPolicy loaded = AnnotationPolicy::load(store, 3000, 300);
    QCOMPARE(loaded.timing(AnnotationTool::Pen).holdMs, 1500);
    for (const auto tool : AnnotationPolicy::supportedTools()) {
        if (tool == AnnotationTool::Pen) {
            continue;
        }
        QCOMPARE(loaded.timing(tool).holdMs, 3000);
        QCOMPARE(loaded.settings(tool).configured.holdMs, Recording::kInherit);
    }
    QCOMPARE(loaded.configuredSize(AnnotationTool::Box), 9);
    QCOMPARE(loaded.configuredSize(AnnotationTool::Circle), Recording::kInherit);
}

void AnnotationPolicyTest::invalidValuesAreRejectedAndNotWritten()
{
    MapStore store;
    const int before = store.writeCount();

    QVERIFY(!AnnotationPolicy::setHoldMs(store, AnnotationTool::Pen,
                                         Recording::kMaxHoldMs + 1));
    QVERIFY(!AnnotationPolicy::setHoldMs(store, AnnotationTool::Pen, -2));
    QVERIFY(!AnnotationPolicy::setFadeMs(store, AnnotationTool::Pen,
                                         Recording::kMaxFadeMs + 1));
    QVERIFY(!AnnotationPolicy::setSize(store, AnnotationTool::Pen,
                                       Recording::kMinSize - 1));
    QVERIFY(!AnnotationPolicy::setSize(store, AnnotationTool::Pen,
                                       Recording::kMaxSize + 1));
    // A tool without a size axis rejects a size even though it is in range.
    QVERIFY(!AnnotationPolicy::setSize(store, AnnotationTool::Invert, 5));
    // One bad member of a pair rejects the pair.
    QVERIFY(!AnnotationPolicy::setTiming(store, AnnotationTool::Pen,
                                         AnnotationTiming{ 1000, -5 }));
    QCOMPARE(store.writeCount(), before);

    // The boundaries themselves are valid.
    QVERIFY(AnnotationPolicy::setHoldMs(store, AnnotationTool::Pen, 0));
    QVERIFY(AnnotationPolicy::setHoldMs(store, AnnotationTool::Pen,
                                        Recording::kMaxHoldMs));
    QVERIFY(AnnotationPolicy::setFadeMs(store, AnnotationTool::Pen, 0));
    QVERIFY(AnnotationPolicy::setSize(store, AnnotationTool::Pen,
                                      Recording::kMaxSize));
    QVERIFY(AnnotationPolicy::setHoldMs(store, AnnotationTool::Pen,
                                        Recording::kInherit));
}

void AnnotationPolicyTest::outOfRangeStoredValuesFallBackToInherit()
{
    MapStore store;
    // A hand-edited file is not range-checked by the policy's writers.
    store.forceValue(QStringLiteral("annotationPenHoldMs"), 9999999);
    store.forceValue(QStringLiteral("annotationPenFadeMs"), -7);
    store.forceValue(QStringLiteral("annotationMarkerSize"), 0);
    store.forceValue(QStringLiteral("annotationTextHoldMs"), QStringLiteral("soon"));

    const AnnotationPolicy loaded = AnnotationPolicy::load(store, 3000, 300);
    QCOMPARE(loaded.settings(AnnotationTool::Pen).configured.holdMs,
             Recording::kInherit);
    QCOMPARE(loaded.timing(AnnotationTool::Pen).holdMs, 3000);
    QCOMPARE(loaded.timing(AnnotationTool::Pen).fadeMs, 300);
    QCOMPARE(loaded.configuredSize(AnnotationTool::Marker), Recording::kInherit);
    QCOMPARE(loaded.timing(AnnotationTool::Text).holdMs, 3000);
}

void AnnotationPolicyTest::counterModeRoundTripsAndValidates()
{
    QCOMPARE(AnnotationPolicy::counterModeIdentifier(
               AnnotationPolicy::defaultCounterMode()),
             QStringLiteral("hold-until-tool-change"));
    QCOMPARE(AnnotationPolicy::counterModeIdentifier(
               AnnotationCounterMode::GroupFromFirstBubble),
             QStringLiteral("group-from-first-bubble"));

    AnnotationCounterMode parsed = AnnotationCounterMode::HoldUntilToolChange;
    QVERIFY(AnnotationPolicy::parseCounterMode(
      QStringLiteral("group-from-first-bubble"), &parsed));
    QCOMPARE(parsed, AnnotationCounterMode::GroupFromFirstBubble);
    QVERIFY(AnnotationPolicy::parseCounterMode(
      QStringLiteral("hold-until-tool-change"), &parsed));
    QCOMPARE(parsed, AnnotationCounterMode::HoldUntilToolChange);
    QVERIFY(!AnnotationPolicy::parseCounterMode(QStringLiteral("per-bubble"),
                                                &parsed));
    QVERIFY(!AnnotationPolicy::parseCounterMode(QString(), &parsed));
    QCOMPARE(parsed, AnnotationCounterMode::HoldUntilToolChange);

    MapStore store;
    QVERIFY(AnnotationPolicy::setCounterMode(
      store, AnnotationCounterMode::GroupFromFirstBubble));
    QCOMPARE(store.read(AnnotationPolicy::counterModeKey()).toString(),
             QStringLiteral("group-from-first-bubble"));

    // An unknown stored mode is not a crash and not a silent switch: the
    // default applies until the user chooses.
    store.forceValue(AnnotationPolicy::counterModeKey(),
                     QStringLiteral("per-bubble"));
    QCOMPARE(AnnotationPolicy::load(store, 3000, 300).counterMode(),
             AnnotationCounterMode::HoldUntilToolChange);

    QVERIFY(AnnotationPolicy::resetCounterMode(store));
    QCOMPARE(AnnotationPolicy::load(store, 3000, 300).counterMode(),
             AnnotationCounterMode::HoldUntilToolChange);
}

void AnnotationPolicyTest::counterEffectiveTiming()
{
    MapStore store;
    QVERIFY(AnnotationPolicy::setTiming(store, AnnotationTool::Counter,
                                        { 2000, 500 }));
    QVERIFY(AnnotationPolicy::setHoldMs(store, AnnotationTool::Pen, 1000));

    // Default mode: the bubbles were visible for as long as the tool was
    // selected, so only the fade is left; everything else keeps its own hold.
    const AnnotationPolicy holdMode = AnnotationPolicy::load(store, 3000, 300);
    QCOMPARE(holdMode.counterMode(),
             AnnotationCounterMode::HoldUntilToolChange);
    QCOMPARE(holdMode.timing(AnnotationTool::Counter).holdMs, 2000);
    QCOMPARE(holdMode.effectiveTiming(AnnotationTool::Counter).holdMs, 0);
    QCOMPARE(holdMode.effectiveTiming(AnnotationTool::Counter).fadeMs, 500);
    QCOMPARE(holdMode.effectiveTiming(AnnotationTool::Pen).holdMs, 1000);

    QVERIFY(AnnotationPolicy::setCounterMode(
      store, AnnotationCounterMode::GroupFromFirstBubble));
    const AnnotationPolicy groupMode = AnnotationPolicy::load(store, 3000, 300);
    QCOMPARE(groupMode.effectiveTiming(AnnotationTool::Counter).holdMs, 2000);
    QCOMPARE(groupMode.effectiveTiming(AnnotationTool::Counter).fadeMs, 500);
}

void AnnotationPolicyTest::counterGroupStamp()
{
    const qint64 own = 10000;
    const qint64 group = 4000;

    // Group timing: every bubble of the group ages from the first one, so the
    // group fades as one instead of bubble by bubble.
    QCOMPARE(AnnotationPolicy::stampFor(
               AnnotationTool::Counter,
               AnnotationCounterMode::GroupFromFirstBubble, own, group),
             group);
    // Default mode: each bubble keeps its own release stamp and the overlay
    // re-stamps the group when the tool changes.
    QCOMPARE(AnnotationPolicy::stampFor(
               AnnotationTool::Counter,
               AnnotationCounterMode::HoldUntilToolChange, own, group),
             own);
    // No group yet.
    QCOMPARE(AnnotationPolicy::stampFor(
               AnnotationTool::Counter,
               AnnotationCounterMode::GroupFromFirstBubble, own, -1),
             own);
    // Only the counter is grouped.
    QCOMPARE(AnnotationPolicy::stampFor(
               AnnotationTool::Pen,
               AnnotationCounterMode::GroupFromFirstBubble, own, group),
             own);
}

void AnnotationPolicyTest::unregisteredKeysAreReportedAndRefused()
{
    // Before the keys are registered in ConfigHandler this is the state the
    // settings section has to survive: no write, no unknown key on disk, and a
    // readable reason.
    MapStore store;
    store.setUnavailable(AnnotationPolicy::configuredKeys());

    const AnnotationPolicy policy = AnnotationPolicy::load(store, 3000, 300);
    QVERIFY(!policy.isFullyAvailable());
    QCOMPARE(int(policy.unavailableKeys().size()),
             int(AnnotationPolicy::configuredKeys().size()));
    QCOMPARE(policy.timing(AnnotationTool::Pen).holdMs, 3000);

    QVERIFY(!AnnotationPolicy::setHoldMs(store, AnnotationTool::Pen, 1000));
    QVERIFY(!AnnotationPolicy::setFadeMs(store, AnnotationTool::Pen, 100));
    QVERIFY(!AnnotationPolicy::setSize(store, AnnotationTool::Pen, 4));
    QVERIFY(!AnnotationPolicy::setCounterMode(
      store, AnnotationCounterMode::GroupFromFirstBubble));
    QVERIFY(!AnnotationPolicy::resetTool(store, AnnotationTool::Pen));
    QVERIFY(!AnnotationPolicy::resetCounterMode(store));
    QVERIFY(store.writes().isEmpty());
}

void AnnotationPolicyTest::resetToolRestoresInheritance()
{
    MapStore store;
    QVERIFY(AnnotationPolicy::setTiming(store, AnnotationTool::Circle, { 900, 90 }));
    QVERIFY(AnnotationPolicy::setSize(store, AnnotationTool::Circle, 6));
    QVERIFY(AnnotationPolicy::resetTool(store, AnnotationTool::Circle));

    const AnnotationPolicy loaded = AnnotationPolicy::load(store, 3000, 300);
    QCOMPARE(loaded.settings(AnnotationTool::Circle).configured.holdMs,
             Recording::kInherit);
    QCOMPARE(loaded.configuredSize(AnnotationTool::Circle), Recording::kInherit);
    QCOMPARE(loaded.timing(AnnotationTool::Circle).holdMs, 3000);
    QCOMPARE(loaded.timing(AnnotationTool::Circle).fadeMs, 300);
}

void AnnotationPolicyTest::sizeResolutionUsesTheDrawingDefault()
{
    // "Default" resolves to the size the drawing tools already use for that
    // tool, so a user who never sets one keeps the drawing behaviour.
    QCOMPARE(AnnotationPolicy::resolveSize(Recording::kInherit, 3), 3);
    QCOMPARE(AnnotationPolicy::resolveSize(7, 3), 7);
    QCOMPARE(AnnotationPolicy::resolveSize(Recording::kMaxSize, 1),
             Recording::kMaxSize);

    MapStore store;
    QVERIFY(AnnotationPolicy::setSize(store, AnnotationTool::Selection, 2));
    const AnnotationPolicy load = AnnotationPolicy::load(store, 3000, 300);
    QCOMPARE(AnnotationPolicy::resolveSize(
               load.configuredSize(AnnotationTool::Selection), 3),
             2);
    QCOMPARE(AnnotationPolicy::resolveSize(
               load.configuredSize(AnnotationTool::Arrow), 3),
             3);
}

QTEST_APPLESS_MAIN(AnnotationPolicyTest)
#include "annotationpolicytest.moc"
