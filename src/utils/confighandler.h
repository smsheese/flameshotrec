// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2017-2019 Alejandro Sirgo Rica & Contributors

#pragma once

#include "widgets/capture/capturetoolbutton.h"

#include <QSettings>
#include <QStringList>
#include <QVariant>
#include <QVector>

#define CONFIG_GROUP_GENERAL "General"
#define CONFIG_GROUP_SHORTCUTS "Shortcuts"

class QFileSystemWatcher;
class ValueHandler;
template<class T>
class QSharedPointer;
class QTextStream;
class AbstractLogger;

/**
 * Declare and implement a getter for a config option. `KEY` is the option key
 * as it appears in the config file, `TYPE` is the C++ type. At the same time
 * `KEY` is the name of the generated getter function.
 */
// clang-format off
#define CONFIG_GETTER(KEY, TYPE)                                               \
    TYPE KEY()                                                                 \
    {                                                                          \
        return value(QStringLiteral(#KEY)).value<TYPE>();                      \
    }
// clang-format on

/**
 * Declare and implement a setter for a config option. `FUNC` is the name of the
 * generated function, `KEY` is the option key as it appears in the config file
 * and `TYPE` is the C++ type.
 */
#define CONFIG_SETTER(FUNC, KEY, TYPE)                                         \
    void FUNC(const TYPE& val)                                                 \
    {                                                                          \
        QString key = QStringLiteral(#KEY);                                    \
        /* Without this check, multiple `flameshot gui` instances running */   \
        /* simultaneously would cause an endless loop of fileWatcher calls */  \
        if (QVariant::fromValue(val) != value(key)) {                          \
            setValue(key, QVariant::fromValue(val));                           \
        }                                                                      \
    }

/**
 * Combines the functionality of `CONFIG_GETTER` and `CONFIG_SETTER`. `GETFUNC`
 * is simultaneously the name of the getter function and the option key as it
 * appears in the config file. `SETFUNC` is the name of the setter function.
 * `TYPE` is the C++ type of the value.
 */
#define CONFIG_GETTER_SETTER(GETFUNC, SETFUNC, TYPE)                           \
    CONFIG_GETTER(GETFUNC, TYPE)                                               \
    CONFIG_SETTER(SETFUNC, GETFUNC, TYPE)

class ConfigHandler : public QObject
{
    Q_OBJECT

public:
    explicit ConfigHandler();

    static ConfigHandler* getInstance();

    // Definitions of getters and setters for config options
    // Some special cases are implemented regularly, without the macro
    // NOTE: When adding new options, make sure to add an entry in
    // recognizedGeneralOptions in the cpp file.
    CONFIG_GETTER_SETTER(userColors, setUserColors, QVector<QColor>);
    QString savePath();
    void setSavePath(const QString& path);
    CONFIG_GETTER_SETTER(savePathFixed, setSavePathFixed, bool)
    CONFIG_GETTER_SETTER(uiLanguage, setUiLanguage, QString)
    CONFIG_GETTER_SETTER(uiColor, setUiColor, QColor)
    CONFIG_GETTER_SETTER(contrastUiColor, setContrastUiColor, QColor)
    CONFIG_GETTER_SETTER(drawColor, setDrawColor, QColor)
    CONFIG_GETTER_SETTER(predefinedColorPaletteLarge,
                         setPredefinedColorPaletteLarge,
                         bool)
    CONFIG_GETTER_SETTER(fontFamily, setFontFamily, QString)
    CONFIG_GETTER_SETTER(showHelp, setShowHelp, bool)
    CONFIG_GETTER_SETTER(showSidePanelButton, setShowSidePanelButton, bool)
    CONFIG_GETTER_SETTER(showDesktopNotification,
                         setShowDesktopNotification,
                         bool)
    CONFIG_GETTER_SETTER(showAbortNotification, setShowAbortNotification, bool)
    CONFIG_GETTER_SETTER(filenamePattern, setFilenamePattern, QString)
    CONFIG_GETTER_SETTER(disabledTrayIcon, setDisabledTrayIcon, bool)
    CONFIG_GETTER_SETTER(drawThickness, setDrawThickness, int)
    CONFIG_GETTER_SETTER(drawFontSize, setDrawFontSize, int)
    CONFIG_GETTER_SETTER(drawCircleCounterSize, setDrawCircleCounterSize, int)
    CONFIG_GETTER_SETTER(drawCircleCounterOutline,
                         setDrawCircleCounterOutline,
                         bool)
    CONFIG_GETTER_SETTER(drawPixelateSize, setDrawPixelateSize, int)
    CONFIG_GETTER_SETTER(drawRectangleSize, setDrawRectangleSize, int)
    CONFIG_GETTER_SETTER(drawMarkerSize, setDrawMarkerSize, int)
    CONFIG_GETTER_SETTER(keepOpenAppLauncher, setKeepOpenAppLauncher, bool)
#if !defined(DISABLE_UPDATE_CHECKER)
    CONFIG_GETTER_SETTER(checkForUpdates, setCheckForUpdates, bool)
#endif
    CONFIG_GETTER_SETTER(allowMultipleGuiInstances,
                         setAllowMultipleGuiInstances,
                         bool)
    CONFIG_GETTER_SETTER(autoCloseIdleDaemon, setAutoCloseIdleDaemon, bool)
    CONFIG_GETTER_SETTER(showStartupLaunchMessage,
                         setShowStartupLaunchMessage,
                         bool)
    CONFIG_GETTER_SETTER(showQuitPrompt, setShowQuitPrompt, bool)
    CONFIG_GETTER_SETTER(contrastOpacity, setContrastOpacity, int)
    CONFIG_GETTER_SETTER(copyURLAfterUpload, setCopyURLAfterUpload, bool)
    CONFIG_GETTER_SETTER(historyConfirmationToDelete,
                         setHistoryConfirmationToDelete,
                         bool)
    CONFIG_GETTER_SETTER(uploadHistoryMax, setUploadHistoryMax, int)
    CONFIG_GETTER_SETTER(saveAfterCopy, setSaveAfterCopy, bool)
    CONFIG_GETTER_SETTER(copyPathAfterSave, setCopyPathAfterSave, bool)
    CONFIG_GETTER_SETTER(saveAsFileExtension, setSaveAsFileExtension, QString)
    CONFIG_GETTER_SETTER(antialiasingPinZoom, setAntialiasingPinZoom, bool)
    CONFIG_GETTER_SETTER(useJpgForClipboard, setUseJpgForClipboard, bool)
#if defined(Q_OS_MACOS)
    CONFIG_GETTER_SETTER(useNativeFullscreen, setUseNativeFullscreen, bool)
#endif
    CONFIG_GETTER_SETTER(uploadWithoutConfirmation,
                         setUploadWithoutConfirmation,
                         bool)
    CONFIG_GETTER_SETTER(ignoreUpdateToVersion,
                         setIgnoreUpdateToVersion,
                         QString)
    CONFIG_GETTER_SETTER(undoLimit, setUndoLimit, int)
    CONFIG_GETTER_SETTER(iaEnabled, setIaEnabled, bool)
    CONFIG_GETTER_SETTER(iaApiUrl, setIaApiUrl, QString)
    CONFIG_GETTER_SETTER(iaApiToken, setIaApiToken, QString)
    CONFIG_GETTER_SETTER(iaModel, setIaModel, QString)
    CONFIG_GETTER_SETTER(iaDefaultMode, setIaDefaultMode, QString)
    CONFIG_GETTER_SETTER(iaCopyResult, setIaCopyResult, bool)
    CONFIG_GETTER_SETTER(iaShowDialog, setIaShowDialog, bool)
    CONFIG_GETTER_SETTER(recordingFfmpegPath, setRecordingFfmpegPath, QString)
    QString recordingVideoPath();
    void setRecordingVideoPath(const QString& path);
    QString recordingFilenamePattern();
    void setRecordingFilenamePattern(const QString& pattern);
    CONFIG_GETTER_SETTER(recordingFrameRate, setRecordingFrameRate, int)
    CONFIG_GETTER_SETTER(recordingToolbarInitialized, setRecordingToolbarInitialized, bool)
    CONFIG_GETTER_SETTER(recordingHoldDuration, setRecordingHoldDuration, int)
    CONFIG_GETTER_SETTER(recordingFadeDuration, setRecordingFadeDuration, int)
    CONFIG_GETTER_SETTER(recordingContainer, setRecordingContainer, QString)
    CONFIG_GETTER_SETTER(recordingCodec, setRecordingCodec, QString)
    CONFIG_GETTER_SETTER(recordingRateControl, setRecordingRateControl, QString)
    CONFIG_GETTER_SETTER(recordingBitrateKbps, setRecordingBitrateKbps, int)
    CONFIG_GETTER_SETTER(recordingQuality, setRecordingQuality, int)
    // System audio is captured from the default output monitor; the
    // microphone is opt-in. Empty source names follow the server defaults.
    CONFIG_GETTER_SETTER(recordingAudioSystemEnabled, setRecordingAudioSystemEnabled, bool)
    CONFIG_GETTER_SETTER(recordingAudioMicEnabled, setRecordingAudioMicEnabled, bool)
    CONFIG_GETTER_SETTER(recordingAudioSystemSource, setRecordingAudioSystemSource, QString)
    CONFIG_GETTER_SETTER(recordingAudioMicSource, setRecordingAudioMicSource, QString)
    CONFIG_GETTER_SETTER(recordingAudioQuality, setRecordingAudioQuality, QString)
    CONFIG_GETTER_SETTER(recordingAudioBitrateKbps, setRecordingAudioBitrateKbps, int)
    CONFIG_GETTER_SETTER(recordingTransferEnabled, setRecordingTransferEnabled, bool)
    CONFIG_GETTER_SETTER(recordingTransferProtocol, setRecordingTransferProtocol, QString)
    CONFIG_GETTER_SETTER(recordingTransferHost, setRecordingTransferHost, QString)
    CONFIG_GETTER_SETTER(recordingTransferPort, setRecordingTransferPort, int)
    CONFIG_GETTER_SETTER(recordingTransferUser, setRecordingTransferUser, QString)
    CONFIG_GETTER_SETTER(recordingTransferRemoteDir, setRecordingTransferRemoteDir, QString)
    CONFIG_GETTER_SETTER(recordingTransferPrivateKey, setRecordingTransferPrivateKey, QString)
    CONFIG_GETTER_SETTER(recordingTransferKnownHosts, setRecordingTransferKnownHosts, QString)
    // Remote storage (shared by screenshots and recordings). The values are read
    // and written through RemoteStorage::ConfigHandlerStore, which uses the
    // generic value()/setValue() API below together with these registrations.
    // Keep the legacy `recordingTransfer*` keys above registered: the one-shot
    // migration in src/remote/remotestorage.cpp reads them and leaves them in
    // place so an older build still understands the file.
    CONFIG_GETTER_SETTER(buttons, setButtons, QList<CaptureTool::Type>)
    CONFIG_GETTER_SETTER(showMagnifier, setShowMagnifier, bool)
    CONFIG_GETTER_SETTER(squareMagnifier, setSquareMagnifier, bool)
    CONFIG_GETTER_SETTER(copyOnDoubleClick, setCopyOnDoubleClick, bool)
    CONFIG_GETTER_SETTER(uploadClientSecret, setUploadClientSecret, QString)
    CONFIG_GETTER_SETTER(saveLastRegion, setSaveLastRegion, bool)
    CONFIG_GETTER_SETTER(showSelectionGeometry, setShowSelectionGeometry, int)
    CONFIG_GETTER_SETTER(jpegQuality, setJpegQuality, int)
    CONFIG_GETTER_SETTER(reverseArrow, setReverseArrow, bool)
    CONFIG_GETTER_SETTER(arrowStyle, setArrowStyle, int)
    CONFIG_GETTER_SETTER(insecurePixelate, setInsecurePixelate, bool)
    CONFIG_GETTER_SETTER(showSelectionGeometryHideTime,
                         showSelectionGeometryHideTime,
                         int)
#if defined(Q_OS_WIN)
    CONFIG_GETTER_SETTER(ignorePrntScrForcesSnipping,
                         setIgnorePrntScrForcesSnipping,
                         bool)
#endif
#if !defined(Q_OS_MACOS)
    CONFIG_GETTER_SETTER(captureActiveMonitor, setCaptureActiveMonitor, bool)
#endif
#if defined(Q_OS_UNIX) && !defined(Q_OS_MACOS)
    CONFIG_GETTER_SETTER(useX11LegacyScreenshot,
                         setUseX11LegacyScreenshot,
                         bool)
#endif

    // SPECIAL CASES
    bool startupLaunch();
    void setStartupLaunch(const bool);
    void setAllTheButtons();
    void setToolSize(CaptureTool::Type toolType, int size);
    int toolSize(CaptureTool::Type toolType);

    // DEFAULTS
    QString filenamePatternDefault();
    void setDefaultSettings();
    QString configFilePath() const;

    // GENERIC GETTERS AND SETTERS
    bool setShortcut(const QString& actionName, const QString& shortcut);
    QString shortcut(const QString& actionName);
    void setValue(const QString& key, const QVariant& value);
    QVariant value(const QString& key) const;
    // Whether the key is present in the file, as opposed to having a value that
    // may legitimately be empty. Used by the remote-storage migration.
    bool contains(const QString& key) const;
    void remove(const QString& key);
    void resetValue(const QString& key);

    // INFO
    static QSet<QString>& recognizedGeneralOptions();
    static QSet<QString>& recognizedShortcutNames();
    QSet<QString> keysFromGroup(const QString& group) const;

    // ERROR HANDLING
    bool checkForErrors(AbstractLogger* log = nullptr) const;
    bool checkUnrecognizedSettings(AbstractLogger* log = nullptr,
                                   QList<QString>* offenders = nullptr) const;
    bool checkShortcutConflicts(AbstractLogger* log = nullptr) const;
    bool checkSemantics(AbstractLogger* log = nullptr,
                        QList<QString>* offenders = nullptr) const;
    void checkAndHandleError() const;
    void setErrorState(bool error) const;
    bool hasError() const;
    QString errorMessage() const;

signals:
    void error() const;
    void errorResolved() const;
    void fileChanged() const;

private:
    mutable QSettings m_settings;

    static bool m_hasError, m_errorCheckPending, m_skipNextErrorCheck;
    static QSharedPointer<QFileSystemWatcher> m_configWatcher;

    void ensureFileWatched() const;
    QSharedPointer<ValueHandler> valueHandler(const QString& key) const;
    void assertKeyRecognized(const QString& key) const;
    bool isShortcut(const QString& key) const;
    QString baseName(const QString& key) const;
    void cleanUnusedKeys(const QString& group, const QSet<QString>& keys) const;
};
