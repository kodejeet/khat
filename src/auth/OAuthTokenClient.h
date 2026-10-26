#pragma once

#include "auth/OAuthToken.h"
#include "config/OAuthClientConfig.h"

#include <QObject>
#include <QNetworkRequest>
#include <QString>
#include <QUrl>
#include <QUrlQuery>

class QNetworkAccessManager;
class QNetworkReply;

// Talks only to the configured token/revocation endpoints. Request IDs let one
// instance handle an authorization exchange and background refreshes without
// mixing up their results.
class OAuthTokenClient : public QObject
{
    Q_OBJECT

public:
    explicit OAuthTokenClient(QNetworkAccessManager *network, QObject *parent = nullptr);

    void setClientConfig(const OAuthClientConfig &config);
    const OAuthClientConfig &clientConfig() const { return m_config; }

    void exchangeAuthorizationCode(const QString &requestId, const QString &code,
                                   const QString &codeVerifier,
                                   const QString &redirectUri);
    void refreshAccessToken(const QString &requestId, const QString &refreshToken);
    void revokeToken(const QString &requestId, const QString &token,
                     const QString &tokenTypeHint = QString());

signals:
    void tokenReceived(const QString &requestId, const OAuthToken &token);
    void tokenRevoked(const QString &requestId);
    void requestFailed(const QString &requestId, const OAuthError &error);

private:
    QNetworkRequest makeRequest(const QUrl &url) const;
    void postForm(const QUrl &url, const QUrlQuery &form, const QString &requestId,
                  bool revoke);
    void handleReply(QNetworkReply *reply, const QString &requestId, bool revoke);
    OAuthError errorFromReply(QNetworkReply *reply, const QByteArray &body) const;

    QNetworkAccessManager *m_network;
    OAuthClientConfig m_config = defaultOAuthClientConfig();
};
