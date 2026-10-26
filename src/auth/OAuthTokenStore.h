#pragma once

#include "auth/OAuthToken.h"

#include <QObject>
#include <QString>

// Persistence seam for refresh tokens. Implementations must never write tokens
// to the config file or logs. An empty "found" result means the account has no
// stored token yet.
class OAuthTokenStore : public QObject
{
    Q_OBJECT

public:
    explicit OAuthTokenStore(QObject *parent = nullptr)
        : QObject(parent)
    {
    }
    ~OAuthTokenStore() override = default;

    virtual void load(const QString &accountId) = 0;
    virtual void save(const QString &accountId, const OAuthToken &token) = 0;
    virtual void remove(const QString &accountId) = 0;

signals:
    void loaded(const QString &accountId, bool found, const OAuthToken &token,
                const QString &error);
    void saved(const QString &accountId, const QString &error);
    void removed(const QString &accountId, const QString &error);
};
