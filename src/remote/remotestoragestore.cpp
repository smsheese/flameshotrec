// SPDX-License-Identifier: GPL-3.0-or-later
#include "remote/remotestoragestore.h"
#include "remote/remotestoragekeys.h"
#include "utils/confighandler.h"

#include <QInputDialog>
#include <QLineEdit>
#include <QVariant>
#include <QWidget>

namespace RemoteStorage {
namespace {

ConfigHandler& configHandler()
{
    return *ConfigHandler::getInstance();
}

} // namespace

QString ConfigHandlerStore::stringValue(const QString& key) const
{
    return configHandler().value(key).toString();
}

int ConfigHandlerStore::intValue(const QString& key, int fallback) const
{
    const QVariant value = configHandler().value(key);
    return value.isValid() ? value.toInt() : fallback;
}

bool ConfigHandlerStore::boolValue(const QString& key, bool fallback) const
{
    const QVariant value = configHandler().value(key);
    return value.isValid() ? value.toBool() : fallback;
}

bool ConfigHandlerStore::contains(const QString& key) const
{
    return configHandler().contains(key);
}

void ConfigHandlerStore::setString(const QString& key, const QString& value)
{
    configHandler().setValue(key, value);
}

void ConfigHandlerStore::setInt(const QString& key, int value)
{
    configHandler().setValue(key, value);
}

void ConfigHandlerStore::setBool(const QString& key, bool value)
{
    configHandler().setValue(key, value);
}

Settings configuredSettings()
{
    ConfigHandlerStore store;
    static bool migrated = false;
    if (!migrated) {
        migrateLegacy(store);
        migrated = true;
    }
    return load(store);
}

void saveConfiguredSettings(const Settings& settings)
{
    ConfigHandlerStore store;
    save(store, settings);
}

bool resolveCredentials(const Connection& connection,
                        Credentials* credentials,
                        QWidget* parent,
                        bool* accepted,
                        QString* error)
{
    if (error) {
        error->clear();
    }
    if (accepted) {
        *accepted = true;
    }
    if (credentials == nullptr) {
        if (error) {
            *error = QStringLiteral("Internal error: no credential destination.");
        }
        return false;
    }
    *credentials = Credentials{};

    const bool needsSecret = connection.protocol == Protocol::S3 ||
                             connection.protocol == Protocol::FTP;
    if (!needsSecret) {
        return true;
    }

    if (connection.secretSource == SecretSource::Environment) {
        bool found = false;
        *credentials = credentialsFromEnvironment(connection, &found);
        if (!found) {
            if (error) {
                *error = connection.protocol == Protocol::S3
                           ? QObject::tr("No S3 secret in the environment. Set %1 (and %2 "
                                         "for a temporary session), or switch the secret "
                                         "source to Prompt.")
                               .arg(QStringLiteral(REMOTE_STORAGE_ENV_S3_SECRET_KEY),
                                    QStringLiteral(REMOTE_STORAGE_ENV_S3_SESSION_TOKEN))
                           : QObject::tr("No FTP password in the environment. Set %1, or "
                                         "switch the secret source to Prompt.")
                               .arg(QStringLiteral(REMOTE_STORAGE_ENV_FTP_PASSWORD));
            }
            return false;
        }
    } else {
        bool entered = false;
        const QString title = QObject::tr("Remote storage");
        const QString label = connection.protocol == Protocol::S3
                                ? QObject::tr("S3 secret access key:")
                                : QObject::tr("FTP password:");
        const QString secret = QInputDialog::getText(
          parent, title, label, QLineEdit::Password, QString(), &entered);
        if (!entered) {
            if (accepted) {
                *accepted = false;
            }
            return false;
        }
        *credentials = credentialsFromSecret(connection, secret);
    }

    if (connection.protocol == Protocol::S3 && credentials->secretAccessKey.isEmpty()) {
        if (error) {
            *error = QObject::tr("The S3 secret access key is empty.");
        }
        return false;
    }
    return true;
}

}
