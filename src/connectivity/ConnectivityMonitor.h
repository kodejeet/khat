#pragma once

#include <QObject>
#include <QString>

#include <optional>

class QNetworkInformation;

class ConnectivityMonitor : public QObject
{
    Q_OBJECT

public:
    enum class State
    {
        Unknown,
        Online,
        Offline,
    };
    Q_ENUM(State)

    explicit ConnectivityMonitor(QObject *parent = nullptr);

    // Loads the Qt network-information backend. Safe to call more than once.
    // If no backend is available, the monitor fails open (Unknown/Online) and
    // request failures still drive scheduler backoff.
    bool initialize();
    bool backendAvailable() const { return m_backendAvailable; }

    State state() const;
    bool isOnline() const;
    QString stateText() const;

    // Deterministic state injection for tests; production uses the backend.
    void setSimulatedState(State state);
    void clearSimulatedState();

signals:
    void stateChanged(ConnectivityMonitor::State state);
    void onlineChanged(bool online);

private:
    void applyBackendState();
    void setState(State state);

    QNetworkInformation *m_networkInformation = nullptr;
    bool m_backendAvailable = false;
    bool m_initialized = false;
    State m_state = State::Unknown;
    std::optional<State> m_simulatedState;
};

Q_DECLARE_METATYPE(ConnectivityMonitor::State)
