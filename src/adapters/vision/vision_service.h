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
//
// m_version/m_healthy are written on the worker thread (runSelfTest) and read
// from the caller thread via version()/isHealthy(); both accessors and the
// writes are guarded by a QMutex so the reads are never torn.

#include <QMutex>
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

    QString version() const override
    {
        QMutexLocker lock(&m_mutex);
        return m_version;
    }
    bool isHealthy() const override
    {
        QMutexLocker lock(&m_mutex);
        return m_healthy;
    }

private slots:
    // Runs on the worker thread: performs the OpenCV version + matrix
    // self-test and emits selfTestPassed()/selfTestFailed().
    void runSelfTest();

private:
    bool m_forceFailure;
    mutable QMutex m_mutex; // guards m_healthy/m_version (worker vs caller)
    bool m_healthy = false;
    QString m_version;
    QThread *m_ownerThread = nullptr;
    QThread *m_thread = nullptr;
};

} // namespace hlm
