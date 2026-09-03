#pragma once

// Port interface for the vision module (spec §6, §7.4, §13). Implemented by
// the OpenCV adapter (Task 9) and later by any camera/algorithm adapter.
//
// The interface is deliberately OpenCV-free: only plain Qt types cross the
// boundary, so OpenCV never leaks into the domain/application layer. The
// self-test runs on the adapter's own worker thread (spec §7.4) and must never
// enter the PLC control path; a failed self-test only marks the vision
// diagnostic red while PLC control keeps working (spec §13).

#include <QObject>
#include <QString>

namespace hlm {

class IVisionService : public QObject
{
    Q_OBJECT

public:
    explicit IVisionService(QObject *parent = nullptr) : QObject(parent) {}
    ~IVisionService() override = default;

    // Starts the adapter's worker thread and runs the OpenCV version + basic
    // matrix self-test there. Results arrive via selfTestPassed() /
    // selfTestFailed(). Safe to call once; stop() before destruction.
    virtual void start() = 0;

    // Stops the worker thread. Safe to call twice.
    virtual void stop() = 0;

    // OpenCV version string (e.g. "4.12.0"), or empty before the self-test
    // has run. Plain QString: no OpenCV types leak through the port.
    virtual QString version() const = 0;

    // True after a successful self-test; false before start() or after a
    // failed self-test. Only affects the vision diagnostic (spec §13).
    virtual bool isHealthy() const = 0;

signals:
    // Emitted from the adapter's worker thread when the self-test passed.
    void selfTestPassed(const QString &version);
    // Emitted from the adapter's worker thread when the self-test failed.
    void selfTestFailed(const QString &reason);
};

} // namespace hlm
