// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "remote/remotestorage.h"

class QWidget;

namespace RemoteStorage {

// Store implementation backed by the application configuration file. Every key
// used here is registered in ConfigHandler::recognizedGeneralOptions().
class ConfigHandlerStore : public Store
{
public:
    QString stringValue(const QString& key) const override;
    int intValue(const QString& key, int fallback) const override;
    bool boolValue(const QString& key, bool fallback) const override;
    bool contains(const QString& key) const override;
    void setString(const QString& key, const QString& value) override;
    void setInt(const QString& key, int value) override;
    void setBool(const QString& key, bool value) override;
};

// Runs the one-shot legacy migration and returns the stored settings.
Settings configuredSettings();

void saveConfiguredSettings(const Settings& settings);

// Resolves the secret for this connection: the environment first when that
// source is selected, otherwise an interactive prompt. `accepted` reports a
// cancelled prompt so the caller can stop without an error.
bool resolveCredentials(const Connection& connection,
                        Credentials* credentials,
                        QWidget* parent,
                        bool* accepted,
                        QString* error);

}
