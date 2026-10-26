#pragma once

#include <QString>

struct AppPaths
{
    QString configDir;
    QString configFile;
    QString oauthClientFile;
    QString stateFile;
    QString runtimeDir;

    static AppPaths defaults();
    static AppPaths fromConfigDir(const QString &configDir);
};
