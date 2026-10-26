#include "auth/SecretServiceTokenStore.h"

#include <QDBusError>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusVariant>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QMap>
#include <QVariantMap>

#include <utility>

namespace {
constexpr auto kService = "org.freedesktop.secrets";
constexpr auto kServicePath = "/org/freedesktop/secrets";
constexpr auto kServiceInterface = "org.freedesktop.Secret.Service";
constexpr auto kCollectionInterface = "org.freedesktop.Secret.Collection";
constexpr auto kItemInterface = "org.freedesktop.Secret.Item";
constexpr int kCallTimeoutMs = 5000;

QString callError(const QString &action, const QDBusError &error)
{
    return QStringLiteral("Secret Service %1 failed: %2").arg(action, error.message());
}

QString newMethodError(const QString &action)
{
    return QStringLiteral("Secret Service %1 failed: the keyring did not return a result.")
        .arg(action);
}
} // namespace

SecretServiceTokenStore::SecretServiceTokenStore(QObject *parent)
    : SecretServiceTokenStore(QDBusConnection::sessionBus(), true, parent)
{
}

SecretServiceTokenStore::SecretServiceTokenStore(const QDBusConnection &connection,
                                                 bool autoStartService, QObject *parent)
    : OAuthTokenStore(parent)
    , m_connection(connection)
    , m_autoStartService(autoStartService)
{
    registerSecretServiceTypes();
}

void SecretServiceTokenStore::load(const QString &accountId)
{
    findItems(accountId, [this, accountId](bool ok,
                                           const QList<QDBusObjectPath> &items,
                                           const QString &error) {
        if (!ok) {
            emit loaded(accountId, false, {}, error);
            return;
        }
        if (items.isEmpty()) {
            emit loaded(accountId, false, {}, QString());
            return;
        }

        QDBusMessage message = QDBusMessage::createMethodCall(
            QString::fromLatin1(kService), QString::fromLatin1(kServicePath),
            QString::fromLatin1(kServiceInterface), QStringLiteral("GetSecrets"));
        message << QVariant::fromValue(items) << QDBusObjectPath(m_sessionPath);
        message.setAutoStartService(m_autoStartService);

        auto *watcher = new QDBusPendingCallWatcher(
            m_connection.asyncCall(message, kCallTimeoutMs), this);
        connect(watcher, &QDBusPendingCallWatcher::finished, this,
                [this, watcher, accountId, items] {
                    QDBusPendingReply<SecretServiceSecrets> reply(*watcher);
                    watcher->deleteLater();
                    if (reply.isError()) {
                        emit loaded(accountId, false, {},
                                    callError(QStringLiteral("token lookup"), reply.error()));
                        return;
                    }

                    const SecretServiceSecrets secrets = reply.argumentAt<0>();

                    for (const QDBusObjectPath &item : items) {
                        const auto it = secrets.constFind(item);
                        if (it == secrets.constEnd()) {
                            continue;
                        }
                        OAuthToken token;
                        QString parseError;
                        QJsonParseError jsonError;
                        const QJsonDocument document =
                            QJsonDocument::fromJson(it->value, &jsonError);
                        if (jsonError.error != QJsonParseError::NoError
                            || !document.isObject()) {
                            emit loaded(accountId, false, {},
                                        QStringLiteral("The stored OAuth token is not valid "
                                                       "JSON."));
                            return;
                        }
                        if (!oauthTokenFromJson(document.object(), &token, &parseError)) {
                            emit loaded(accountId, false, {}, parseError);
                            return;
                        }
                        emit loaded(accountId, true, token, QString());
                        return;
                    }

                    emit loaded(accountId, false, {},
                                QStringLiteral("Secret Service did not return the stored "
                                               "token."));
                });
    });
}

void SecretServiceTokenStore::save(const QString &accountId, const OAuthToken &token)
{
    if ((!token.hasAccessToken() && !token.hasRefreshToken())) {
        emit saved(accountId, QStringLiteral("Refusing to store an empty OAuth token."));
        return;
    }

    ensureSession([this, accountId, token](bool ok, const QString &error) {
        if (!ok) {
            emit saved(accountId, error);
            return;
        }

        QDBusMessage aliasMessage = QDBusMessage::createMethodCall(
            QString::fromLatin1(kService), QString::fromLatin1(kServicePath),
            QString::fromLatin1(kServiceInterface), QStringLiteral("ReadAlias"));
        aliasMessage << QStringLiteral("default");
        aliasMessage.setAutoStartService(m_autoStartService);

        auto *aliasWatcher = new QDBusPendingCallWatcher(
            m_connection.asyncCall(aliasMessage, kCallTimeoutMs), this);
        connect(aliasWatcher, &QDBusPendingCallWatcher::finished, this,
                [this, aliasWatcher, accountId, token] {
                    QDBusPendingReply<QDBusObjectPath> aliasReply(*aliasWatcher);
                    aliasWatcher->deleteLater();
                    if (aliasReply.isError()) {
                        emit saved(accountId,
                                   callError(QStringLiteral("collection lookup"),
                                             aliasReply.error()));
                        return;
                    }

                    const QDBusObjectPath collection = aliasReply.argumentAt<0>();
                    if (collection.path().isEmpty() || collection.path() == QStringLiteral("/")) {
                        emit saved(accountId,
                                   QStringLiteral("No default Secret Service collection is "
                                                  "available."));
                        return;
                    }

                    QMap<QString, QString> attributes;
                    attributes.insert(QStringLiteral("application"),
                                      QStringLiteral("khat"));
                    attributes.insert(QStringLiteral("account"), accountId);
                    attributes.insert(QStringLiteral("type"), QStringLiteral("oauth-token"));

                    QVariantMap properties;
                    properties.insert(QStringLiteral("org.freedesktop.Secret.Item.Label"),
                                      QStringLiteral("khat token for %1")
                                          .arg(accountId));
                    properties.insert(QStringLiteral("org.freedesktop.Secret.Item.Attributes"),
                                      QVariant::fromValue(attributes));

                    SecretServiceSecret secret;
                    secret.session = QDBusObjectPath(m_sessionPath);
                    secret.value = QJsonDocument(oauthTokenToJson(token))
                                       .toJson(QJsonDocument::Compact);
                    secret.contentType = QStringLiteral("application/json");

                    QDBusMessage message = QDBusMessage::createMethodCall(
                        QString::fromLatin1(kService), collection.path(),
                        QString::fromLatin1(kCollectionInterface),
                        QStringLiteral("CreateItem"));
                    message << properties << QVariant::fromValue(secret) << true;
                    message.setAutoStartService(m_autoStartService);

                    auto *watcher = new QDBusPendingCallWatcher(
                        m_connection.asyncCall(message, kCallTimeoutMs), this);
                    connect(watcher, &QDBusPendingCallWatcher::finished, this,
                            [this, watcher, accountId] {
                                QDBusPendingReply<QDBusObjectPath, QDBusObjectPath> reply(
                                    *watcher);
                                watcher->deleteLater();
                                if (reply.isError()) {
                                    emit saved(accountId,
                                               callError(QStringLiteral("token save"),
                                                         reply.error()));
                                    return;
                                }
                                const QDBusObjectPath item = reply.argumentAt<0>();
                                const QDBusObjectPath prompt = reply.argumentAt<1>();
                                if (item.path().isEmpty()
                                    || item.path() == QStringLiteral("/")) {
                                    emit saved(accountId,
                                               QStringLiteral("Secret Service did not create a "
                                                              "token item."));
                                    return;
                                }
                                if (!prompt.path().isEmpty()
                                    && prompt.path() != QStringLiteral("/")) {
                                    emit saved(accountId,
                                               QStringLiteral("Secret Service requires user "
                                                              "interaction to save the token."));
                                    return;
                                }
                                emit saved(accountId, QString());
                            });
                });
    });
}

void SecretServiceTokenStore::remove(const QString &accountId)
{
    findItems(accountId, [this, accountId](bool ok,
                                           const QList<QDBusObjectPath> &items,
                                           const QString &error) {
        if (!ok) {
            emit removed(accountId, error);
            return;
        }
        if (items.isEmpty()) {
            emit removed(accountId, QString());
            return;
        }
        deleteNext(items, 0, QString(), accountId);
    });
}

void SecretServiceTokenStore::ensureSession(SessionCallback callback)
{
    if (!m_connection.isConnected()) {
        callback(false, QStringLiteral("The session D-Bus is not available."));
        return;
    }
    if (m_sessionReady) {
        callback(true, QString());
        return;
    }

    m_sessionWaiters.append(std::move(callback));
    if (!m_opening) {
        m_opening = true;
        openSession();
    }
}

void SecretServiceTokenStore::openSession()
{
    QDBusMessage message = QDBusMessage::createMethodCall(
        QString::fromLatin1(kService), QString::fromLatin1(kServicePath),
        QString::fromLatin1(kServiceInterface), QStringLiteral("OpenSession"));
    message << QStringLiteral("plain") << QVariant::fromValue(QDBusVariant(QString()));
    message.setAutoStartService(m_autoStartService);

    auto *watcher = new QDBusPendingCallWatcher(
        m_connection.asyncCall(message, kCallTimeoutMs), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher] {
        QDBusPendingReply<QDBusVariant, QDBusObjectPath> reply(*watcher);
        watcher->deleteLater();
        m_opening = false;

        bool ok = false;
        QString error;
        if (reply.isError()) {
            error = callError(QStringLiteral("session setup"), reply.error());
        } else {
            const QDBusObjectPath session = reply.argumentAt<1>();
            if (session.path().isEmpty() || session.path() == QStringLiteral("/")) {
                error = newMethodError(QStringLiteral("session setup"));
            } else {
                m_sessionPath = session.path();
                m_sessionReady = true;
                ok = true;
            }
        }

        const QList<SessionCallback> waiters = std::exchange(m_sessionWaiters, {});
        for (const SessionCallback &callback : waiters) {
            callback(ok, error);
        }
    });
}

void SecretServiceTokenStore::findItems(const QString &accountId, ItemsCallback callback)
{
    ensureSession([this, accountId, callback = std::move(callback)](
                      bool ok, const QString &sessionError) {
        if (!ok) {
            callback(false, {}, sessionError);
            return;
        }

        QMap<QString, QString> attributes;
        attributes.insert(QStringLiteral("application"), QStringLiteral("khat"));
        attributes.insert(QStringLiteral("account"), accountId);

        QDBusMessage message = QDBusMessage::createMethodCall(
            QString::fromLatin1(kService), QString::fromLatin1(kServicePath),
            QString::fromLatin1(kServiceInterface), QStringLiteral("SearchItems"));
        message << QVariant::fromValue(attributes);
        message.setAutoStartService(m_autoStartService);

        auto *watcher = new QDBusPendingCallWatcher(
            m_connection.asyncCall(message, kCallTimeoutMs), this);
        connect(watcher, &QDBusPendingCallWatcher::finished, this,
                [this, watcher, callback = std::move(callback)](QDBusPendingCallWatcher *) {
                    QDBusPendingReply<QList<QDBusObjectPath>, QList<QDBusObjectPath>> reply(
                        *watcher);
                    watcher->deleteLater();
                    if (reply.isError()) {
                        callback(false, {},
                                 callError(QStringLiteral("token search"), reply.error()));
                        return;
                    }

                    const QList<QDBusObjectPath> unlocked = reply.argumentAt<0>();
                    const QList<QDBusObjectPath> locked = reply.argumentAt<1>();
                    if (locked.isEmpty()) {
                        callback(true, unlocked, QString());
                        return;
                    }
                    unlockItems(locked, unlocked, std::move(callback));
                });
    });
}

void SecretServiceTokenStore::unlockItems(const QList<QDBusObjectPath> &locked,
                                          const QList<QDBusObjectPath> &unlocked,
                                          ItemsCallback callback)
{
    QDBusMessage message = QDBusMessage::createMethodCall(
        QString::fromLatin1(kService), QString::fromLatin1(kServicePath),
        QString::fromLatin1(kServiceInterface), QStringLiteral("Unlock"));
    message << QVariant::fromValue(locked);
    message.setAutoStartService(m_autoStartService);

    auto *watcher = new QDBusPendingCallWatcher(
        m_connection.asyncCall(message, kCallTimeoutMs), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this, watcher, locked, unlocked, callback = std::move(callback)](
                QDBusPendingCallWatcher *) {
                QDBusPendingReply<QList<QDBusObjectPath>, QDBusObjectPath> reply(*watcher);
                watcher->deleteLater();
                if (reply.isError()) {
                    callback(false, {},
                             callError(QStringLiteral("keyring unlock"), reply.error()));
                    return;
                }

                const QList<QDBusObjectPath> newlyUnlocked = reply.argumentAt<0>();
                const QDBusObjectPath prompt = reply.argumentAt<1>();
                if (newlyUnlocked.size() < locked.size()) {
                    Q_UNUSED(prompt)
                    callback(false, {},
                             QStringLiteral("The Secret Service keyring is locked. Unlock it "
                                            "and try again."));
                    return;
                }

                QList<QDBusObjectPath> all = unlocked;
                all += newlyUnlocked;
                callback(true, all, QString());
            });
}

void SecretServiceTokenStore::deleteNext(const QList<QDBusObjectPath> &items, int index,
                                         QString firstError, const QString &accountId)
{
    if (index >= items.size()) {
        emit removed(accountId, firstError);
        return;
    }

    QDBusMessage message = QDBusMessage::createMethodCall(
        QString::fromLatin1(kService), items.at(index).path(),
        QString::fromLatin1(kItemInterface), QStringLiteral("Delete"));
    message.setAutoStartService(m_autoStartService);

    auto *watcher = new QDBusPendingCallWatcher(
        m_connection.asyncCall(message, kCallTimeoutMs), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this, watcher, items, index, firstError, accountId] {
                QDBusPendingReply<QDBusObjectPath> reply(*watcher);
                watcher->deleteLater();
                QString error = firstError;
                if (reply.isError() && error.isEmpty()) {
                    error = callError(QStringLiteral("token removal"), reply.error());
                } else {
                    const QDBusObjectPath prompt = reply.argumentAt<0>();
                    if (error.isEmpty() && !prompt.path().isEmpty()
                        && prompt.path() != QStringLiteral("/")) {
                        error = QStringLiteral("Secret Service requires user interaction to "
                                               "remove the token.");
                    }
                }
                deleteNext(items, index + 1, error, accountId);
            });
}
