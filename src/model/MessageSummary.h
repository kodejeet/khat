#pragma once

#include <QMetaType>
#include <QString>
#include <QStringList>

struct MessageSummary
{
    QString id;
    QString threadId;
    qint64 internalDate = 0; // Gmail internalDate, milliseconds since epoch.
    QString sender;
    QString subject;
    QString snippet;
    QStringList labelIds;
};

Q_DECLARE_METATYPE(MessageSummary)
