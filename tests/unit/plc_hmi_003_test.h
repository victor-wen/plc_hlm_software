// Intentionally unused shim, superseded before the RED run.
//
// An earlier authoring pass put naming-tolerant quality/age accessors here so
// that the PLC-HMI-003 tests would compile under both the contract spelling and
// the pre-revision camelCase spelling. The AUTHOR-phase directive requires
// tests to use the contract-fixed snapshot names directly
// (fast_quality, fast_age_ms, home_quality, home_age_ms, command_quality,
// command_age_ms, slow_quality, slow_age_ms, overall_quality, overall_age_ms,
// width_delta_valid) and to fail to compile on the current tree instead.
//
// No test source includes this file. The filesystem sandbox has no delete
// permission, so the shim is emptied rather than removed.
