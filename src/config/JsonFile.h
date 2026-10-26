#pragma once

#include <QJsonObject>
#include <QString>

enum class JsonReadResult
{
    Ok,
    Missing,
    Error,
};

JsonReadResult readJsonObjectFile(const QString &path, QJsonObject *object, QString *error);
bool writeJsonObjectFile(const QString &path, const QJsonObject &object, QString *error);
