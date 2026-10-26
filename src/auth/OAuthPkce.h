#pragma once

#include "config/OAuthClientConfig.h"

#include <QString>
#include <QUrl>

// PKCE values for one authorization attempt. The verifier and state are
// secrets for the lifetime of the flow and must not be written to logs.
struct OAuthPkce
{
    QString verifier;
    QString state;
    QString challenge;

    static OAuthPkce generate();

    // RFC 7636 S256 challenge: BASE64URL(SHA256(ASCII(verifier))).
    static QString codeChallenge(const QString &verifier);
};

QUrl buildOAuthAuthorizationUrl(const OAuthClientConfig &client,
                                const QString &redirectUri, const OAuthPkce &pkce);
