#include "config/JsonFile.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSaveFile>

namespace {
constexpr qint64 kMaxConfigBytes = 1024 * 1024;
}

JsonReadResult readJsonObjectFile(const QString &path, QJsonObject *object, QString *error)
{
    QFile file(path);
    if (!file.exists()) {
        return JsonReadResult::Missing;
    }
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) {
            *error = QStringLiteral("Cannot read %1: %2").arg(path, file.errorString());
        }
        return JsonReadResult::Error;
    }

    const QByteArray data = file.read(kMaxConfigBytes + 1);
    if (data.size() > kMaxConfigBytes) {
        if (error) {
            *error = QStringLiteral("Refusing to read oversized file: %1").arg(path);
        }
        return JsonReadResult::Error;
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(data, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (error) {
            *error = QStringLiteral("Invalid JSON in %1 at offset %2.")
                         .arg(path)
                         .arg(parseError.offset);
        }
        return JsonReadResult::Error;
    }

    *object = document.object();
    return JsonReadResult::Ok;
}

bool writeJsonObjectFile(const QString &path, const QJsonObject &object, QString *error)
{
    const QFileInfo info(path);
    QDir directory = info.absoluteDir();
    if (!directory.exists() && !directory.mkpath(QStringLiteral("."))) {
        if (error) {
            *error = QStringLiteral("Cannot create directory: %1").arg(directory.absolutePath());
        }
        return false;
    }

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error) {
            *error = QStringLiteral("Cannot write %1: %2").arg(path, file.errorString());
        }
        return false;
    }

    const QByteArray data = QJsonDocument(object).toJson(QJsonDocument::Indented);
    if (file.write(data) != data.size()) {
        if (error) {
            *error = QStringLiteral("Cannot write %1: %2").arg(path, file.errorString());
        }
        file.cancelWriting();
        return false;
    }
    if (!file.commit()) {
        if (error) {
            *error = QStringLiteral("Cannot commit %1: %2").arg(path, file.errorString());
        }
        return false;
    }

    // Settings and OAuth client configuration are readable only by the owner.
    QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner);
    return true;
}
