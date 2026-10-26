#pragma once

#include <QDBusConnection>
#include <QObject>

// Listens for logind's PrepareForSleep signal so a suspended session pauses
// polling and resumes with an immediate check. Missing or inaccessible system
// D-Bus is non-fatal; the notifier still runs normally.
class PowerMonitor : public QObject
{
    Q_OBJECT

public:
    explicit PowerMonitor(QObject *parent = nullptr);
    explicit PowerMonitor(const QDBusConnection &connection, QObject *parent = nullptr);

    bool subscribe(QString *error = nullptr);
    bool isSubscribed() const { return m_subscribed; }

signals:
    void aboutToSuspend();
    void resumed();

private slots:
    void handlePrepareForSleep(bool start);

private:
    QDBusConnection m_connection;
    bool m_subscribed = false;
};
