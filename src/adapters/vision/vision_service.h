#pragma once

// OpenCV implementation of IVisionService (spec §6, §7.4, §13).
//
// The self-test (OpenCV version + basic matrix arithmetic) runs on the
// adapter's own worker thread (spec §7.4): future camera capture and algorithm
// processing must stay on this thread and never enter the PLC control path.
// A failed self-test only marks the vision diagnostic red; PLC control keeps
// working (spec §13).
//
// The service must be created WITHOUT a parent so it can be moved to the
// worker thread. Call stop() before destroying it.

#include <QObject>
#include <QString>

#include "ports/ivision_service.h"

class QThread;

namespace hlm {

class VisionService : public IVisionService
{
    Q_OBJECT

public:
    // `forceSelfTestFailure` injects a self-test failure (test-only hook) so
    // the "self-test failed" path is deterministic and testable.
    explicit VisionService(bool forceSelfTestFailure = false,
                           QObject *parent = nullptr);
    ~VisionService() override;

    void start() override;
    void stop() override;

    QString version() const override { return m_version; }
    bool isHealthy() const override { return m_healthy; }

private slots:
    // Runs on the worker thread: performs the OpenCV version + matrix
    // self-test and emits selfTestPassed()/selfTestFailed().
    void runSelfTest();

private:
    bool m_forceFailure;
    bool m_healthy = false;
    QString m_version;
    QThread *m_thread = nullptr;
};

} // namespace hlm
