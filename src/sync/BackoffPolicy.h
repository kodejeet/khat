#pragma once

class BackoffPolicy
{
public:
    explicit BackoffPolicy(int baseDelayMs = 60'000, int capDelayMs = 30 * 60'000);

    // Returns the delay after another failure. A server Retry-After hint can
    // extend the delay, but never beyond the cap.
    int nextDelayMs(int retryAfterSeconds = 0);
    void reset();

    int failureCount() const { return m_failureCount; }
    int baseDelayMs() const { return m_baseDelayMs; }
    int capDelayMs() const { return m_capDelayMs; }

private:
    int m_baseDelayMs;
    int m_capDelayMs;
    int m_failureCount = 0;
};
