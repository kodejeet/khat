#include "sync/BackoffPolicy.h"

#include <QtTest>

class TestBackoff : public QObject
{
    Q_OBJECT

private slots:
    void exponentialSequence();
    void capsAtMaximum();
    void retryAfterExtendsButCaps();
    void resetStartsOver();
};

void TestBackoff::exponentialSequence()
{
    BackoffPolicy policy(60'000, 30 * 60'000);
    QCOMPARE(policy.failureCount(), 0);
    QCOMPARE(policy.nextDelayMs(), 60'000);
    QCOMPARE(policy.nextDelayMs(), 120'000);
    QCOMPARE(policy.nextDelayMs(), 240'000);
    QCOMPARE(policy.nextDelayMs(), 480'000);
    QCOMPARE(policy.failureCount(), 4);
}

void TestBackoff::capsAtMaximum()
{
    BackoffPolicy policy(60'000, 30 * 60'000);
    for (int i = 0; i < 20; ++i) {
        policy.nextDelayMs();
    }
    QCOMPARE(policy.nextDelayMs(), 30 * 60'000);
    QCOMPARE(policy.nextDelayMs(), 30 * 60'000);
}

void TestBackoff::retryAfterExtendsButCaps()
{
    BackoffPolicy policy(60'000, 30 * 60'000);
    QCOMPARE(policy.nextDelayMs(120), 120'000);
    QCOMPARE(policy.nextDelayMs(), 120'000);
    QCOMPARE(policy.nextDelayMs(3600), 30 * 60'000);
}

void TestBackoff::resetStartsOver()
{
    BackoffPolicy policy(60'000, 30 * 60'000);
    policy.nextDelayMs();
    policy.nextDelayMs();
    QCOMPARE(policy.failureCount(), 2);
    policy.reset();
    QCOMPARE(policy.failureCount(), 0);
    QCOMPARE(policy.nextDelayMs(), 60'000);
}

QTEST_GUILESS_MAIN(TestBackoff)
#include "test_backoff.moc"
