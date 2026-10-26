#include "config/ConfigStore.h"

#include "config/JsonFile.h"

#include <QJsonArray>
#include <QJsonValue>

#include <utility>

namespace {
bool readString(const QJsonObject &object, const QString &key, QString *value,
                QStringList *errors)
{
    const QJsonValue json = object.value(key);
    if (json.isUndefined()) {
        return true;
    }
    if (!json.isString()) {
        if (errors) {
            *errors << QStringLiteral("%1 must be a string.").arg(key);
        }
        return false;
    }
    *value = json.toString();
    return true;
}

bool readBool(const QJsonObject &object, const QString &key, bool *value, QStringList *errors)
{
    const QJsonValue json = object.value(key);
    if (json.isUndefined()) {
        return true;
    }
    if (!json.isBool()) {
        if (errors) {
            *errors << QStringLiteral("%1 must be a boolean.").arg(key);
        }
        return false;
    }
    *value = json.toBool();
    return true;
}

bool readInt(const QJsonObject &object, const QString &key, int *value, QStringList *errors)
{
    const QJsonValue json = object.value(key);
    if (json.isUndefined()) {
        return true;
    }
    if (!json.isDouble()) {
        if (errors) {
            *errors << QStringLiteral("%1 must be an integer.").arg(key);
        }
        return false;
    }
    const double raw = json.toDouble();
    if (raw != static_cast<double>(static_cast<int>(raw))) {
        if (errors) {
            *errors << QStringLiteral("%1 must be an integer.").arg(key);
        }
        return false;
    }
    *value = static_cast<int>(raw);
    return true;
}
} // namespace

bool appConfigFromJson(const QJsonObject &object, AppConfig *config, QStringList *errors)
{
    QStringList localErrors;
    AppConfig parsed = defaultAppConfig();

    readInt(object, QStringLiteral("schema_version"), &parsed.schemaVersion, &localErrors);

    const QJsonValue accountsValue = object.value(QStringLiteral("accounts"));
    if (!accountsValue.isUndefined()) {
        if (!accountsValue.isArray()) {
            localErrors << QStringLiteral("accounts must be an array.");
        } else {
            const QJsonArray accounts = accountsValue.toArray();
            for (const QJsonValue &entry : accounts) {
                if (!entry.isObject()) {
                    localErrors << QStringLiteral("Each account must be an object.");
                    continue;
                }
                const QJsonObject accountObject = entry.toObject();
                AccountConfig account;
                readString(accountObject, QStringLiteral("id"), &account.id, &localErrors);
                readString(accountObject, QStringLiteral("email"), &account.email, &localErrors);
                readString(accountObject, QStringLiteral("display_name"), &account.displayName,
                           &localErrors);
                readBool(accountObject, QStringLiteral("enabled"), &account.enabled, &localErrors);
                parsed.accounts.append(account);
            }
        }
    }

    const QJsonValue notificationsValue = object.value(QStringLiteral("notifications"));
    if (!notificationsValue.isUndefined()) {
        if (!notificationsValue.isObject()) {
            localErrors << QStringLiteral("notifications must be an object.");
        } else {
            const QJsonObject notifications = notificationsValue.toObject();
            readBool(notifications, QStringLiteral("enabled"), &parsed.notifications.enabled,
                     &localErrors);
            readInt(notifications, QStringLiteral("poll_interval_minutes"),
                    &parsed.notifications.pollIntervalMinutes, &localErrors);
            readBool(notifications, QStringLiteral("include_updates"),
                     &parsed.notifications.includeUpdates, &localErrors);
            readBool(notifications, QStringLiteral("show_preview"),
                     &parsed.notifications.showPreview, &localErrors);
        }
    }

    localErrors += validateAppConfig(parsed);
    if (!localErrors.isEmpty()) {
        if (errors) {
            *errors = localErrors;
        }
        return false;
    }

    *config = parsed;
    return true;
}

QJsonObject appConfigToJson(const AppConfig &config)
{
    QJsonObject root;
    root.insert(QStringLiteral("schema_version"), config.schemaVersion);

    QJsonArray accounts;
    for (const AccountConfig &account : config.accounts) {
        QJsonObject object;
        object.insert(QStringLiteral("id"), account.id);
        object.insert(QStringLiteral("email"), account.email);
        object.insert(QStringLiteral("display_name"), account.displayName);
        object.insert(QStringLiteral("enabled"), account.enabled);
        accounts.append(object);
    }
    root.insert(QStringLiteral("accounts"), accounts);

    QJsonObject notifications;
    notifications.insert(QStringLiteral("enabled"), config.notifications.enabled);
    notifications.insert(QStringLiteral("poll_interval_minutes"),
                         config.notifications.pollIntervalMinutes);
    notifications.insert(QStringLiteral("include_updates"), config.notifications.includeUpdates);
    notifications.insert(QStringLiteral("show_preview"), config.notifications.showPreview);
    root.insert(QStringLiteral("notifications"), notifications);

    return root;
}

ConfigStore::ConfigStore(QString path)
    : m_path(std::move(path))
{
}

bool ConfigStore::load(QString *error)
{
    QJsonObject object;
    const JsonReadResult readResult = readJsonObjectFile(m_path, &object, error);
    if (readResult == JsonReadResult::Missing) {
        m_config = defaultAppConfig();
        return true;
    }
    if (readResult == JsonReadResult::Error) {
        return false;
    }

    AppConfig parsed;
    QStringList errors;
    if (!appConfigFromJson(object, &parsed, &errors)) {
        if (error) {
            *error = errors.join(QStringLiteral(" "));
        }
        return false;
    }

    m_config = parsed;
    return true;
}

bool ConfigStore::save(const AppConfig &config, QString *error)
{
    const QStringList errors = validateAppConfig(config);
    if (!errors.isEmpty()) {
        if (error) {
            *error = errors.join(QStringLiteral(" "));
        }
        return false;
    }
    if (!writeJsonObjectFile(m_path, appConfigToJson(config), error)) {
        return false;
    }
    m_config = config;
    return true;
}
