#pragma once

#include "auth/OAuthTokenStore.h"
#include "auth/SecretServiceTypes.h"

#include <QDBusConnection>
#include <QList>
#include <QString>

#include <functional>

// Stores one JSON token blob per account as a Secret Service item. QtDBus is
// used directly so no libsecret development package is required.
class SecretServiceTokenStore : public OAuthTokenStore
{
    Q_OBJECT

public:
    explicit SecretServiceTokenStore(QObject *parent = nullptr);
    SecretServiceTokenStore(const QDBusConnection &connection, bool autoStartService,
                            QObject *parent = nullptr);

    void load(const QString &accountId) override;
    void save(const QString &accountId, const OAuthToken &token) override;
    void remove(const QString &accountId) override;

    bool isConnected() const { return m_connection.isConnected(); }

private:
    using SessionCallback = std::function<void(bool ok, const QString &error)>;
    using ItemsCallback =
        std::function<void(bool ok, const QList<QDBusObjectPath> &items,
                           const QString &error)>;

    void ensureSession(SessionCallback callback);
    void openSession();
    void findItems(const QString &accountId, ItemsCallback callback);
    void unlockItems(const QList<QDBusObjectPath> &locked, const QList<QDBusObjectPath> &unlocked,
                     ItemsCallback callback);
    void deleteNext(const QList<QDBusObjectPath> &items, int index, QString firstError,
                    const QString &accountId);

    QDBusConnection m_connection;
    bool m_autoStartService;
    QString m_sessionPath;
    bool m_sessionReady = false;
    bool m_opening = false;
    QList<SessionCallback> m_sessionWaiters;
};
