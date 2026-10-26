#include "config/OAuthClientStore.h"

#include "config/JsonFile.h"

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
} // namespace

bool oauthClientConfigFromJson(const QJsonObject &object, OAuthClientConfig *config,
                               QStringList *errors)
{
    QStringList localErrors;
    OAuthClientConfig parsed = defaultOAuthClientConfig();

    static const QStringList forbiddenKeys = {
        QStringLiteral("access_token"),
        QStringLiteral("refresh_token"),
        QStringLiteral("id_token"),
        QStringLiteral("token"),
        QStringLiteral("expires_in"),
    };
    for (const QString &key : forbiddenKeys) {
        if (object.contains(key)) {
            localErrors << QStringLiteral("%1 must not be stored in the OAuth client file; "
                                          "tokens belong in Secret Service.")
                               .arg(key);
        }
    }

    readString(object, QStringLiteral("client_id"), &parsed.clientId, &localErrors);
    readString(object, QStringLiteral("client_secret"), &parsed.clientSecret, &localErrors);

    QString authUri;
    if (readString(object, QStringLiteral("auth_uri"), &authUri, &localErrors)
        && !authUri.isEmpty()) {
        parsed.authUri = QUrl(authUri);
    }
    QString tokenUri;
    if (readString(object, QStringLiteral("token_uri"), &tokenUri, &localErrors)
        && !tokenUri.isEmpty()) {
        parsed.tokenUri = QUrl(tokenUri);
    }
    QString revokeUri;
    if (readString(object, QStringLiteral("revoke_uri"), &revokeUri, &localErrors)
        && !revokeUri.isEmpty()) {
        parsed.revokeUri = QUrl(revokeUri);
    }
    readString(object, QStringLiteral("scope"), &parsed.scope, &localErrors);

    localErrors += validateOAuthClientConfig(parsed);
    if (!localErrors.isEmpty()) {
        if (errors) {
            *errors = localErrors;
        }
        return false;
    }

    *config = parsed;
    return true;
}

QJsonObject oauthClientConfigToJson(const OAuthClientConfig &config)
{
    QJsonObject root;
    root.insert(QStringLiteral("client_id"), config.clientId);
    root.insert(QStringLiteral("client_secret"), config.clientSecret);
    root.insert(QStringLiteral("auth_uri"), config.authUri.toString());
    root.insert(QStringLiteral("token_uri"), config.tokenUri.toString());
    root.insert(QStringLiteral("revoke_uri"), config.revokeUri.toString());
    root.insert(QStringLiteral("scope"), config.scope);
    return root;
}

OAuthClientStore::OAuthClientStore(QString path)
    : m_path(std::move(path))
{
}

bool OAuthClientStore::load(QString *error)
{
    QJsonObject object;
    const JsonReadResult readResult = readJsonObjectFile(m_path, &object, error);
    if (readResult == JsonReadResult::Missing) {
        if (error) {
            *error = QStringLiteral("OAuth client configuration not found at %1. See "
                                    "config/oauth-client.example.json.")
                         .arg(m_path);
        }
        return false;
    }
    if (readResult == JsonReadResult::Error) {
        return false;
    }

    OAuthClientConfig parsed;
    QStringList errors;
    if (!oauthClientConfigFromJson(object, &parsed, &errors)) {
        if (error) {
            *error = errors.join(QStringLiteral(" "));
        }
        return false;
    }

    m_config = parsed;
    return true;
}

bool OAuthClientStore::save(const OAuthClientConfig &config, QString *error)
{
    const QStringList errors = validateOAuthClientConfig(config);
    if (!errors.isEmpty()) {
        if (error) {
            *error = errors.join(QStringLiteral(" "));
        }
        return false;
    }
    if (!writeJsonObjectFile(m_path, oauthClientConfigToJson(config), error)) {
        return false;
    }
    m_config = config;
    return true;
}
