// SPDX-License-Identifier: GPL-3.0-or-later
#include "recordtool.h"

RecordTool::RecordTool(QObject* parent, Type type) : AbstractActionTool(parent), m_type(type) {}
bool RecordTool::closeOnButtonPressed() const { return false; }
QIcon RecordTool::icon(const QColor& background, bool inEditor) const
{
    Q_UNUSED(inEditor)
    return QIcon(iconPath(background) + (m_type == TYPE_RECORD_UPLOAD ? "cloud-upload.svg" : "record.svg"));
}
QString RecordTool::name() const { return m_type == TYPE_RECORD_UPLOAD ? tr("Upload capture") : tr("Record"); }
QString RecordTool::description() const { return m_type == TYPE_RECORD_UPLOAD ? tr("Save locally and upload via FTP/SFTP (configure in Video settings)") : tr("Record the selected region"); }
CaptureTool::Type RecordTool::type() const { return m_type; }
CaptureTool* RecordTool::copy(QObject* parent) { return new RecordTool(parent, m_type); }
void RecordTool::pressed(CaptureContext& context)
{
    Q_UNUSED(context)
    if (m_type == TYPE_RECORD) emit requestAction(REQ_START_RECORDING);
}
