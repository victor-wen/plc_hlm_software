#pragma once

#include <QString>

// Command now lives in the domain layer; including it here re-exports the
// enum for every existing includer of this header (application and UI code).
#include "domain/command.h"

namespace hlm {

// User roles (spec §11.4).
enum class Role { Anonymous, Operator, Admin };

// Structured permission result: no magic booleans (spec §11.4).
struct PermissionResult {
    bool allowed = false;
    QString reason; // empty when allowed
};

// Pure role-based permission matrix (spec §11.4). No state, no I/O.
class PermissionPolicy
{
public:
    static PermissionResult check(Role role, Command cmd);
};

} // namespace hlm
