#pragma once

#include "config/OAuthClientConfig.h"

#include <QJsonObject>
#include <QString>

bool oauthClientConfigFromJson(const QJsonObject &object, OAuthClientConfig *config,
                               QStringList *errors);
QJsonObject oauthClientConfigToJson(const OAuthClientConfig &config);

class OAuthClientStore
{
public:
    explicit OAuthClientStore(QString path);

    QString path() const { return m_path; }
    const OAuthClientConfig &config() const { return m_config; }

    // Missing, malformed, or placeholder configuration is an error: this file
    // must be created by the user and is never generated with fake credentials.
    bool load(QString *error = nullptr);
    bool save(const OAuthClientConfig &config, QString *error = nullptr);

private:
    QString m_path;
    OAuthClientConfig m_config = defaultOAuthClientConfig();
};
