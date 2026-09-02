// Task 8 unit tests: PBKDF2-HMAC-SHA256 password derivation (spec §11.5, §17).
// Vectors are the RFC 6070 test cases (password="password", salt="salt").

#include <QtTest>

#include "domain/password_derivation.h"

using namespace hlm;

class PasswordDerivationTest : public QObject
{
    Q_OBJECT

private slots:
    void rfc6070VectorIteration1();
    void rfc6070VectorIteration2();
    void rfc6070VectorIteration4096();
    void saltIsRandomAndDistinct();
    void derivedKeyLengthIs32Bytes();
    void constantTimeEqualsDetectsDifference();
    void emptySaltFails();
};

void PasswordDerivationTest::rfc6070VectorIteration1()
{
    const QByteArray key = derivePasswordKey(QStringLiteral("password"),
                                             QByteArrayLiteral("salt"), 1);
    QCOMPARE(passwordHashHex(key),
             QStringLiteral("120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b"));
}

void PasswordDerivationTest::rfc6070VectorIteration2()
{
    const QByteArray key = derivePasswordKey(QStringLiteral("password"),
                                             QByteArrayLiteral("salt"), 2);
    QCOMPARE(passwordHashHex(key),
             QStringLiteral("ae4d0c95af6b46d32d0adff928f06dd02a303f8ef3c251dfd6e2d85a95474c43"));
}

void PasswordDerivationTest::rfc6070VectorIteration4096()
{
    const QByteArray key = derivePasswordKey(QStringLiteral("password"),
                                             QByteArrayLiteral("salt"), 4096);
    QCOMPARE(passwordHashHex(key),
             QStringLiteral("c5e478d59288c841aa530db6845c4c8d962893a001ce4e11a4963873aa98134a"));
}

void PasswordDerivationTest::saltIsRandomAndDistinct()
{
    const QByteArray a = generateSalt();
    const QByteArray b = generateSalt();
    QCOMPARE(a.size(), kSaltBytes);
    QCOMPARE(b.size(), kSaltBytes);
    QVERIFY(a != b); // 16 random bytes colliding is effectively impossible
}

void PasswordDerivationTest::derivedKeyLengthIs32Bytes()
{
    const QByteArray key = derivePasswordKey(QStringLiteral("hunter2"),
                                             generateSalt(), 1000);
    QCOMPARE(key.size(), kDerivedKeyBytes);
    QCOMPARE(passwordHashHex(key).size(), kDerivedKeyBytes * 2);
}

void PasswordDerivationTest::constantTimeEqualsDetectsDifference()
{
    QVERIFY(constantTimeEquals(QStringLiteral("abc"), QStringLiteral("abc")));
    QVERIFY(!constantTimeEquals(QStringLiteral("abc"), QStringLiteral("abd")));
    QVERIFY(!constantTimeEquals(QStringLiteral("abc"), QStringLiteral("abcd")));
    QVERIFY(!constantTimeEquals(QString(), QStringLiteral("a")));
}

void PasswordDerivationTest::emptySaltFails()
{
    QVERIFY(derivePasswordKey(QStringLiteral("password"), QByteArray(), 1000).isEmpty());
}

QTEST_GUILESS_MAIN(PasswordDerivationTest)
#include "test_password_derivation.moc"
