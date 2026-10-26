#pragma once

#include <QString>
#include <QUrl>

struct OAuthClientConfig
{
    QString clientId;
    QString clientSecret;
    QUrl authUri;
    QUrl tokenUri;
    QUrl revokeUri;
    QString scope;
};

OAuthClientConfig defaultOAuthClientConfig();

// Returns an empty list when the configuration is valid.
QStringList validateOAuthClientConfig(const OAuthClientConfig &config);

bool isPlaceholderValue(const QString &value);
