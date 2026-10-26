#pragma once

#include <QDateTime>
#include <QJsonObject>
#include <QString>

// Short-lived access token plus the long-lived refresh token returned by
// Google. This struct is only serialized into Secret Service, never into the
// application configuration or logs.
struct OAuthToken
{
    QString accessToken;
    QString refreshToken;
    QString tokenType = QStringLiteral("Bearer");
    QString scope;
    QDateTime expiresAt;

    bool hasAccessToken() const { return !accessToken.isEmpty(); }
    bool hasRefreshToken() const { return !refreshToken.isEmpty(); }
    bool isAccessTokenExpired(const QDateTime &now = QDateTime::currentDateTimeUtc()) const
    {
        return expiresAt.isValid() && expiresAt <= now;
    }
};

QJsonObject oauthTokenToJson(const OAuthToken &token);
bool oauthTokenFromJson(const QJsonObject &object, OAuthToken *token,
                        QString *error = nullptr);

enum class OAuthErrorKind
{
    None,
    Network,
    InvalidRequest,
    InvalidGrant,
    Server,
    InvalidResponse,
    Revoked,
};

struct OAuthError
{
    OAuthErrorKind kind = OAuthErrorKind::None;
    int httpStatus = 0;
    QString code;
    QString message;

    bool requiresReauthorization() const
    {
        return kind == OAuthErrorKind::InvalidGrant || kind == OAuthErrorKind::Revoked;
    }
};

Q_DECLARE_METATYPE(OAuthToken)
Q_DECLARE_METATYPE(OAuthError)
