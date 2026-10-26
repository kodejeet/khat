#include "auth/OAuthToken.h"

#include <QJsonValue>
#include <QTimeZone>

QJsonObject oauthTokenToJson(const OAuthToken &token)
{
    QJsonObject object;
    object.insert(QStringLiteral("access_token"), token.accessToken);
    object.insert(QStringLiteral("refresh_token"), token.refreshToken);
    object.insert(QStringLiteral("token_type"),
                  token.tokenType.isEmpty() ? QStringLiteral("Bearer") : token.tokenType);
    object.insert(QStringLiteral("scope"), token.scope);
    object.insert(QStringLiteral("expires_at"),
                  token.expiresAt.isValid() ? token.expiresAt.toSecsSinceEpoch() : 0);
    return object;
}

bool oauthTokenFromJson(const QJsonObject &object, OAuthToken *token, QString *error)
{
    auto fail = [error](const QString &message) {
        if (error) {
            *error = message;
        }
        return false;
    };

    if (!token) {
        return fail(QStringLiteral("No token output was provided."));
    }
    for (const QString &key : {QStringLiteral("access_token"), QStringLiteral("refresh_token"),
                               QStringLiteral("token_type"), QStringLiteral("scope")}) {
        const QJsonValue value = object.value(key);
        if (!value.isUndefined() && !value.isString()) {
            return fail(QStringLiteral("Stored OAuth token field %1 is invalid.").arg(key));
        }
    }
    const QJsonValue expires = object.value(QStringLiteral("expires_at"));
    if (!expires.isUndefined() && !expires.isDouble() && !expires.isString()) {
        return fail(QStringLiteral("Stored OAuth token expiry is invalid."));
    }

    OAuthToken parsed;
    parsed.accessToken = object.value(QStringLiteral("access_token")).toString();
    parsed.refreshToken = object.value(QStringLiteral("refresh_token")).toString();
    parsed.tokenType = object.value(QStringLiteral("token_type")).toString();
    if (parsed.tokenType.isEmpty()) {
        parsed.tokenType = QStringLiteral("Bearer");
    }
    parsed.scope = object.value(QStringLiteral("scope")).toString();

    qint64 expiresAt = 0;
    if (expires.isDouble()) {
        expiresAt = static_cast<qint64>(expires.toDouble());
    } else if (expires.isString()) {
        expiresAt = expires.toString().toLongLong();
    }
    if (expiresAt > 0) {
        parsed.expiresAt = QDateTime::fromSecsSinceEpoch(expiresAt, QTimeZone::UTC);
    }

    if (parsed.accessToken.isEmpty() && parsed.refreshToken.isEmpty()) {
        return fail(QStringLiteral("Stored OAuth token has no access or refresh token."));
    }

    *token = parsed;
    return true;
}
