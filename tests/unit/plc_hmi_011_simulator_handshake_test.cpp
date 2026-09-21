// PLC-HMI-011: intentionally unused duplicate stub.
//
// This file originally contained six OB-8 cases (the simulated-controller
// home-start handshake) that duplicated, case for case, the six OB-8 cases
// already present and running in the registered target
// tests/unit/operator_command_lifecycle_test.cpp:
//   resetPulseAloneDoesNotStartHoming,
//   homeStartWriteStartsHomingAndReadsBack,
//   homingCompletionClearsTheStartBitAndSetsHomeComplete,
//   resetPulseStillClearsHomeCompleteFaultAndWidthAdjustment,
//   gatewayResetPulseAloneLeavesHomingIdle,
//   gatewayReportsTheHandshakeThroughSnapshots.
// It added no coverage beyond the registered file and was never registered in
// tests/unit/CMakeLists.txt, so it could not build or run.
//
// The test engineer resolved the duplicate by removing it from the build set:
// the sandbox denies file deletion (rm), so, following the established
// tests/unit/plc_hmi_003_test.h precedent, the file was emptied to this
// comment-only stub rather than left as a second unregistered copy. No case,
// assertion, expectation, or coverage entry was weakened, skipped, disabled,
// or removed from the registered target; nothing includes this file.