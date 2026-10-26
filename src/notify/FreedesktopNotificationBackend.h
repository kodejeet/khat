#pragma once

#include "notify/NotificationBackend.h"

#include <QDBusConnection>
#include <QHash>
#include <QString>

class FreedesktopNotificationBackend : public NotificationBackend
{
    Q_OBJECT

public:
    explicit FreedesktopNotificationBackend(QObject *parent = nullptr);
    explicit FreedesktopNotificationBackend(const QDBusConnection &connection,
                                            bool autoStartService = true,
                                            QObject *parent = nullptr);
    ~FreedesktopNotificationBackend() override;

    bool notify(const NotificationContent &content, QString *error = nullptr) override;

signals:
    void notificationSent(uint id);

private slots:
    void handleActionInvoked(uint id, const QString &action);
    void handleNotificationClosed(uint id, uint reason);

private:
    void connectToServer();

    QDBusConnection m_connection;
    QString m_serviceName = QStringLiteral("org.freedesktop.Notifications");
    QString m_objectPath = QStringLiteral("/org/freedesktop/Notifications");
    QHash<uint, QString> m_urls;
    bool m_autoStartService = true;
};
