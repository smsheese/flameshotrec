// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Flameshot IA Contributors

#pragma once

#include <QDialog>
#include <QString>

class QComboBox;
class QPlainTextEdit;

class AIDialog : public QDialog
{
    Q_OBJECT

public:
    explicit AIDialog(const QString& defaultMode,
                      const QString& defaultPrompt,
                      QWidget* parent = nullptr);

    QString mode() const;
    QString prompt() const;

    // Hand-off of one answered prompt. The AI tool asks the user while the
    // capture editor is still open, because a cancelled prompt must leave the
    // editor untouched; the AI backend starts afterwards, once the editor is
    // dismissed. The tool stores the answer, the backend reads it once and
    // never asks the user again.
    static void setPendingAnswer(const AIDialog& answeredDialog);
    static QString pendingAnswerMode();
    static QString pendingAnswerPrompt();
    static void clearPendingAnswer();

private:
    void onModeChanged(int index);
    void updatePlaceholder();

    QComboBox* m_modeCombo;
    QPlainTextEdit* m_promptEdit;

    struct ModePreset
    {
        QString id;
        QString label;
        QString placeholder;
    };

    QVector<ModePreset> m_presets;
};
