#include "connectivity/ConnectivityMonitor.h"

#include <QNetworkInformation>

ConnectivityMonitor::ConnectivityMonitor(QObject *parent)
    : QObject(parent)
{
    qRegisterMetaType<ConnectivityMonitor::State>("ConnectivityMonitor::State");
}

bool ConnectivityMonitor::initialize()
{
    if (m_initialized) {
        return m_backendAvailable;
    }
    m_initialized = true;

    if (!QNetworkInformation::loadDefaultBackend()) {
        m_backendAvailable = false;
        setState(State::Unknown);
        return false;
    }

    m_networkInformation = QNetworkInformation::instance();
    m_backendAvailable = m_networkInformation != nullptr;
    if (!m_backendAvailable) {
        setState(State::Unknown);
        return false;
    }

    connect(m_networkInformation, &QNetworkInformation::reachabilityChanged, this,
            [this](QNetworkInformation::Reachability) { applyBackendState(); });
    applyBackendState();
    return true;
}

ConnectivityMonitor::State ConnectivityMonitor::state() const
{
    if (m_simulatedState) {
        return *m_simulatedState;
    }
    return m_state;
}

bool ConnectivityMonitor::isOnline() const
{
    // Unknown fails open so a missing backend or a transient unknown state does
    // not block the first launch. Actual request failures drive backoff.
    return state() != State::Offline;
}

QString ConnectivityMonitor::stateText() const
{
    switch (state()) {
    case State::Online:
        return QStringLiteral("Online");
    case State::Offline:
        return QStringLiteral("Offline");
    case State::Unknown:
        return QStringLiteral("Unknown");
    }
    return QStringLiteral("Unknown");
}

void ConnectivityMonitor::setSimulatedState(State state)
{
    if (m_simulatedState && *m_simulatedState == state) {
        return;
    }
    const bool wasOnline = isOnline();
    m_simulatedState = state;
    const bool nowOnline = isOnline();
    emit stateChanged(state);
    if (wasOnline != nowOnline) {
        emit onlineChanged(nowOnline);
    }
}

void ConnectivityMonitor::clearSimulatedState()
{
    if (!m_simulatedState) {
        return;
    }
    const bool wasOnline = isOnline();
    m_simulatedState.reset();
    const bool nowOnline = isOnline();
    emit stateChanged(state());
    if (wasOnline != nowOnline) {
        emit onlineChanged(nowOnline);
    }
}

void ConnectivityMonitor::applyBackendState()
{
    if (!m_networkInformation) {
        setState(State::Unknown);
        return;
    }

    switch (m_networkInformation->reachability()) {
    case QNetworkInformation::Reachability::Online:
    case QNetworkInformation::Reachability::Site:
        setState(State::Online);
        break;
    case QNetworkInformation::Reachability::Local:
    case QNetworkInformation::Reachability::Disconnected:
        setState(State::Offline);
        break;
    case QNetworkInformation::Reachability::Unknown:
        setState(State::Unknown);
        break;
    }
}

void ConnectivityMonitor::setState(State state)
{
    if (m_state == state) {
        return;
    }
    const bool wasOnline = isOnline();
    m_state = state;
    const bool nowOnline = isOnline();
    if (!m_simulatedState) {
        emit stateChanged(state);
    }
    if (wasOnline != nowOnline) {
        emit onlineChanged(nowOnline);
    }
}
