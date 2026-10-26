#include "sync/BackoffPolicy.h"

#include <QtGlobal>

BackoffPolicy::BackoffPolicy(int baseDelayMs, int capDelayMs)
    : m_baseDelayMs(qMax(1, baseDelayMs))
    , m_capDelayMs(qMax(m_baseDelayMs, capDelayMs))
{
}

int BackoffPolicy::nextDelayMs(int retryAfterSeconds)
{
    ++m_failureCount;
    const int exponent = qMin(m_failureCount - 1, 20);

    qint64 delay = m_baseDelayMs;
    for (int i = 0; i < exponent && delay < m_capDelayMs; ++i) {
        delay *= 2;
    }
    delay = qMin<qint64>(delay, m_capDelayMs);

    if (retryAfterSeconds > 0) {
        const qint64 hint = static_cast<qint64>(retryAfterSeconds) * 1000;
        delay = qMax(delay, hint);
        delay = qMin<qint64>(delay, m_capDelayMs);
    }

    return static_cast<int>(qBound<qint64>(1, delay, 2'147'483'647LL));
}

void BackoffPolicy::reset()
{
    m_failureCount = 0;
}
