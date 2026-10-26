#include "config/AppPaths.h"

#include <QDir>
#include <QStandardPaths>

#include <unistd.h>

namespace {
QString configBaseDir()
{
    QString base = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    if (base.isEmpty()) {
        base = QDir::homePath() + QStringLiteral("/.config");
    }
    return base;
}

QString dataBaseDir()
{
    QString base = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    if (base.isEmpty()) {
        base = QDir::homePath() + QStringLiteral("/.local/share");
    }
    return base;
}

QString runtimeBaseDir()
{
    QString base = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    if (base.isEmpty()) {
        base = QDir::tempPath() + QStringLiteral("/khat-")
            + QString::number(static_cast<qulonglong>(::getuid()));
    }
    return base;
}
} // namespace

AppPaths AppPaths::defaults()
{
    return fromConfigDir(configBaseDir() + QStringLiteral("/khat"));
}

AppPaths AppPaths::fromConfigDir(const QString &configDir)
{
    AppPaths paths;
    paths.configDir = QDir::cleanPath(configDir);
    paths.configFile = paths.configDir + QStringLiteral("/config.json");
    paths.oauthClientFile = paths.configDir + QStringLiteral("/oauth-client.json");
    paths.stateFile = dataBaseDir() + QStringLiteral("/khat/state.db");
    paths.runtimeDir = runtimeBaseDir() + QStringLiteral("/khat");
    return paths;
}
