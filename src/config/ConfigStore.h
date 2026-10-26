#pragma once

#include "config/AppConfig.h"

#include <QJsonObject>
#include <QString>

bool appConfigFromJson(const QJsonObject &object, AppConfig *config, QStringList *errors);
QJsonObject appConfigToJson(const AppConfig &config);

class ConfigStore
{
public:
    explicit ConfigStore(QString path);

    QString path() const { return m_path; }
    const AppConfig &config() const { return m_config; }

    // A missing file is treated as a first launch and yields defaults.
    bool load(QString *error = nullptr);
    bool save(const AppConfig &config, QString *error = nullptr);

private:
    QString m_path;
    AppConfig m_config = defaultAppConfig();
};
