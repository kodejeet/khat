#pragma once

#include "notify/NotificationContent.h"

#include <QObject>

// Abstraction so delivery and click handling can be tested without a desktop
// notification server.
class NotificationBackend : public QObject
{
    Q_OBJECT

public:
    explicit NotificationBackend(QObject *parent = nullptr)
        : QObject(parent)
    {
    }
    ~NotificationBackend() override = default;

    // Returns true when the notification was accepted for asynchronous delivery.
    virtual bool notify(const NotificationContent &content, QString *error = nullptr) = 0;

signals:
    void activated(const QString &url);
    void failed(const QString &error);
};
