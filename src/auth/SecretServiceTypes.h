#pragma once

#include <QByteArray>
#include <QDBusArgument>
#include <QDBusObjectPath>
#include <QList>
#include <QMap>
#include <QMetaType>
#include <QString>

// Wire representation of the Secret Service "Secret" struct:
// (o session, ay parameters, ay value, s content_type).
struct SecretServiceSecret
{
    QDBusObjectPath session;
    QByteArray parameters;
    QByteArray value;
    QString contentType;
};

Q_DECLARE_METATYPE(SecretServiceSecret)
using SecretServiceAttributes = QMap<QString, QString>;
using SecretServiceSecrets = QMap<QDBusObjectPath, SecretServiceSecret>;
Q_DECLARE_METATYPE(SecretServiceAttributes)
Q_DECLARE_METATYPE(SecretServiceSecrets)
Q_DECLARE_METATYPE(QList<QDBusObjectPath>)

QDBusArgument &operator<<(QDBusArgument &argument, const SecretServiceSecret &secret);
const QDBusArgument &operator>>(const QDBusArgument &argument, SecretServiceSecret &secret);

void registerSecretServiceTypes();
