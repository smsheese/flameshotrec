// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Flameshot IA Contributors

#include "aitool.h"
#include "utils/confighandler.h"
#include "widgets/ai/aidialog.h"

#include <QApplication>
#include <QDialog>

AITool::AITool(QObject* parent)
  : AbstractActionTool(parent)
{}

bool AITool::closeOnButtonPressed() const
{
    return true;
}

QIcon AITool::icon(const QColor& background, bool inEditor) const
{
    Q_UNUSED(inEditor)
    return QIcon(iconPath(background) + "ai.svg");
}

QString AITool::name() const
{
    return tr("AI");
}

CaptureTool::Type AITool::type() const
{
    return CaptureTool::TYPE_AI;
}

QString AITool::description() const
{
    return tr("Analyze screenshot with AI");
}

CaptureTool* AITool::copy(QObject* parent)
{
    return new AITool(parent);
}

void AITool::pressed(CaptureContext& context)
{
    ConfigHandler config;
    if (!config.iaEnabled()) {
        return;
    }

    // A new press starts a new cycle: an answer stored by an earlier press must
    // never be reused if this one is cancelled.
    AIDialog::clearPendingAnswer();

    QString mode = config.iaDefaultMode();
    if (mode.isEmpty()) {
        mode = QStringLiteral("ask");
    }

    // Ask first, change the editor only afterwards: a cancelled prompt has to
    // leave the capture and its selection exactly as the user left them.
    if (config.iaShowDialog()) {
        AIDialog dialog(mode, QString(), QApplication::activeWindow());
        // Do not rely on transient stacking above the always-on-top overlay:
        // raise and focus it explicitly before blocking in exec().
        dialog.raise();
        dialog.activateWindow();
        if (dialog.exec() != QDialog::Accepted) {
            return;
        }
        // The AI backend starts after the editor is gone, so it picks the
        // answer up from here instead of asking the user a second time.
        AIDialog::setPendingAnswer(dialog);
    }

    // Dismiss the editor right away: the answer arrives in its own window, so
    // the overlay must not stay on top of it. The export (and with it the AI
    // request) runs once, from the editor's teardown.

    emit requestAction(REQ_CLEAR_SELECTION);
    context.request.addTask(CaptureRequest::AI);
    emit requestAction(REQ_CAPTURE_DONE_OK);
    emit requestAction(REQ_CLOSE_GUI);
}
