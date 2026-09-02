// Task 1 smoke test: verifies the build baseline.
//
// - hlm_core links and its version symbol resolves (target link smoke test).
// - The version string is non-empty and matches the CMake project version.
// - CTest discovers and runs this test (enable_testing + add_test).

#include <QtTest>

#include "common/version.h"

class SmokeTest : public QObject
{
    Q_OBJECT

private slots:
    void versionStringIsNonEmpty();
    void versionStringMatchesProjectVersion();
};

void SmokeTest::versionStringIsNonEmpty()
{
    QVERIFY(hlm::versionString() != nullptr);
    QVERIFY(qstrlen(hlm::versionString()) > 0);
}

void SmokeTest::versionStringMatchesProjectVersion()
{
    QCOMPARE(QString::fromLatin1(hlm::versionString()), QStringLiteral(HLM_VERSION_STRING));
}

QTEST_GUILESS_MAIN(SmokeTest)
#include "test_smoke.moc"
