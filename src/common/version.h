#pragma once

namespace hlm {

// Compile-time application version, injected via HLM_VERSION_STRING.
const char *versionString() noexcept;

} // namespace hlm
