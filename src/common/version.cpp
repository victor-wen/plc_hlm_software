// Version string for the PLC HMI application.
//
// Task 1 placeholder: hlm_core needs at least one translation unit so the
// target links. Later tasks fill src/common/ with the real shared utilities.

namespace hlm {

const char *versionString() noexcept
{
    return HLM_VERSION_STRING;
}

} // namespace hlm
