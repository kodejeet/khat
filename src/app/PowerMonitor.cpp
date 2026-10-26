#include "app/PowerMonitor.h"

#include <QDBusConnectionInterface>

PowerMonitor::PowerMonitor(QObject *parent)
    : PowerMonitor(QDBusConnection::systemBus(), parent)
{
}

PowerMonitor::PowerMonitor(const QDBusConnection &connection, QObject *parent)
    : QObject(parent)
    , m_connection(connection)
{
}

bool PowerMonitor::subscribe(QString *error)
{
    if (m_subscribed) {
        return true;
    }
    if (!m_connection.isConnected()) {
        if (error) {
            *error = QStringLiteral("The system D-Bus is not available.");
        }
        return false;
    }
    if (!m_connection.interface()
             || !m_connection.interface()->isServiceRegistered(
                 QStringLiteral("org.freedesktop.login1"))) {
        if (error) {
            *error = QStringLiteral("logind is not available on the system D-Bus.");
        }
        return false;
    }

    m_subscribed = m_connection.connect(
        QStringLiteral("org.freedesktop.login1"),
        QStringLiteral("/org/freedesktop/login1"),
        QStringLiteral("org.freedesktop.login1.Manager"),
        QStringLiteral("PrepareForSleep"), this,
        SLOT(handlePrepareForSleep(bool)));
    if (!m_subscribed) {
        if (error) {
            *error = QStringLiteral("Cannot subscribe to logind sleep events: %1")
                         .arg(m_connection.lastError().message());
        }
    }
    return m_subscribed;
}

void PowerMonitor::handlePrepareForSleep(bool start)
{
    if (start) {
        emit aboutToSuspend();
    } else {
        emit resumed();
    }
}
