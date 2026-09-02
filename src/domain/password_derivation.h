#pragma once

// PBKDF2-HMAC-SHA256 password derivation (spec §11.5, §17).
//
// - 16-byte independent random salt, 32-byte derived key, versioned
//   parameters; default 600,000 iterations.
// - No custom algorithms, no plaintext storage: only the hex digest of the
//   derived key is ever persisted or logged.

#include <QByteArray>
#include <QString>

namespace hlm {

// Default PBKDF2 iteration count (spec §11.5).
inline constexpr int kDefaultPbkdf2Iterations = 600000;
inline constexpr int kSaltBytes = 16;
inline constexpr int kDerivedKeyBytes = 32;

// Derives a 32-byte key from `password` and `salt` using PBKDF2-HMAC-SHA256.
// Returns an empty QByteArray on failure (e.g. OpenSSL error).
QByteArray derivePasswordKey(const QString &password, const QByteArray &salt,
                             int iterations);

// Generates kSaltBytes cryptographically random bytes. Empty on failure.
QByteArray generateSalt();

// Hex digest (lowercase) of the derived key; this is what is stored in the
// users table. Never log the raw key.
QString passwordHashHex(const QByteArray &derivedKey);

// Constant-time comparison of two hex digests (avoids timing side channels).
bool constantTimeEquals(const QString &a, const QString &b);

} // namespace hlm
