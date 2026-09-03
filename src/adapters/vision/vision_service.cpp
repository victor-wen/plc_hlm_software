#include "adapters/vision/vision_service.h"

#include <QThread>

#include <opencv2/core.hpp>

namespace hlm {

namespace {

// Deterministic basic matrix self-test (spec §6: 基础矩阵运算自检):
//   A = [1 2; 3 4], B = A * A = [7 10; 15 22], det(B) = 7*22 - 10*15 = 4.
// Exact double arithmetic on these small integer-valued matrices, so the
// check is deterministic across platforms and OpenCV builds.
bool runMatrixSelfTest()
{
    cv::Mat a = (cv::Mat_<double>(2, 2) << 1.0, 2.0, 3.0, 4.0);
    const cv::Mat b = a * a;
    if (b.rows != 2 || b.cols != 2)
        return false;
    const double det = cv::determinant(b);
    return std::abs(det - 4.0) < 1e-9;
}

} // namespace

VisionService::VisionService(bool forceSelfTestFailure, QObject *parent)
    : IVisionService(parent), m_forceFailure(forceSelfTestFailure)
{
}

VisionService::~VisionService()
{
    stop();
}

void VisionService::start()
{
    if (m_thread)
        return;
    m_thread = new QThread(this);
    // The self-test runs on the adapter's own worker thread (spec §7.4);
    // future camera capture and algorithm processing must stay here and never
    // enter the PLC control path.
    moveToThread(m_thread);
    connect(m_thread, &QThread::started, this, &VisionService::runSelfTest);
    m_thread->start();
}

void VisionService::stop()
{
    if (!m_thread)
        return;
    m_thread->quit();
    m_thread->wait();
    delete m_thread;
    m_thread = nullptr;
}

void VisionService::runSelfTest()
{
    m_version = QString::fromUtf8(cv::getVersionString().c_str());
    if (m_forceFailure) {
        m_healthy = false;
        emit selfTestFailed(QStringLiteral("injected self-test failure"));
        return;
    }
    if (m_version.isEmpty() || !runMatrixSelfTest()) {
        m_healthy = false;
        emit selfTestFailed(QStringLiteral("OpenCV version or matrix self-test failed"));
        return;
    }
    m_healthy = true;
    emit selfTestPassed(m_version);
}

} // namespace hlm
