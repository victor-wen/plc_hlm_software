#include "domain/password_derivation.h"

#include <openssl/evp.h>
#include <openssl/rand.h>

#include <QByteArray>

namespace hlm {

QByteArray derivePasswordKey(const QString &password, const QByteArray &salt,
                             int iterations)
{
    if (iterations < 1 || salt.size() < 1)
        return {};
    const QByteArray pw = password.toUtf8();
    QByteArray out(kDerivedKeyBytes, Qt::Uninitialized);
    const int rc = PKCS5_PBKDF2_HMAC(pw.constData(), pw.size(),
                                     reinterpret_cast<const unsigned char *>(salt.constData()),
                                     salt.size(), iterations, EVP_sha256(),
                                     out.size(),
                                     reinterpret_cast<unsigned char *>(out.data()));
    if (rc != 1)
        return {};
    return out;
}

QByteArray generateSalt()
{
    QByteArray salt(kSaltBytes, Qt::Uninitialized);
    if (RAND_bytes(reinterpret_cast<unsigned char *>(salt.data()), salt.size()) != 1)
        return {};
    return salt;
}

QString passwordHashHex(const QByteArray &derivedKey)
{
    return QString::fromLatin1(derivedKey.toHex());
}

bool constantTimeEquals(const QString &a, const QString &b)
{
    if (a.size() != b.size())
        return false;
    // XOR accumulation over the UTF-16 code units; runs in constant time
    // regardless of where the first difference occurs.
    quint16 acc = 0;
    for (int i = 0; i < a.size(); ++i)
        acc |= static_cast<quint16>(a[i].unicode() ^ b[i].unicode());
    return acc == 0;
}

} // namespace hlm
