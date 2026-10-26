#include "auth/SecretServiceTypes.h"

#include <QDBusMetaType>

QDBusArgument &operator<<(QDBusArgument &argument, const SecretServiceSecret &secret)
{
    argument.beginStructure();
    argument << secret.session << secret.parameters << secret.value << secret.contentType;
    argument.endStructure();
    return argument;
}

const QDBusArgument &operator>>(const QDBusArgument &argument, SecretServiceSecret &secret)
{
    argument.beginStructure();
    argument >> secret.session >> secret.parameters >> secret.value >> secret.contentType;
    argument.endStructure();
    return argument;
}

void registerSecretServiceTypes()
{
    static const bool registered = [] {
        qRegisterMetaType<SecretServiceAttributes>();
        qRegisterMetaType<SecretServiceSecrets>();
        qRegisterMetaType<QList<QDBusObjectPath>>();
        qDBusRegisterMetaType<SecretServiceAttributes>();
        qDBusRegisterMetaType<SecretServiceSecrets>();
        qDBusRegisterMetaType<SecretServiceSecret>();
        return true;
    }();
    Q_UNUSED(registered)
}
