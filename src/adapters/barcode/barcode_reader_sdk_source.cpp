#include "adapters/barcode/barcode_reader_sdk_source.h"

#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QMutexLocker>
#include <QTimer>

#ifdef _WIN32
#include <windows.h>
#endif

namespace hlm {

namespace {

// Vendor-recommended response buffer (README_CN.md:28): 1 MiB + 1. The SDK's
// requiredBytes includes the trailing NUL.
constexpr int kResponseCapacity = 1024 * 1024 + 1;
// The SDK blocks in the calling thread; its own C/C++ examples pass 2000 ms.
constexpr unsigned int kCallTimeoutMs = 2000;
// The vendor's sample polls every 50 ms and allows 35 s per cycle
// (TriggerClient.cs:62,73; README_CN.md:109). A board is either in the cached
// frame by then or it is not.
constexpr int kPollIntervalMs = 50;
constexpr qint64 kCycleDeadlineMs = 35000;
// Largest response this adapter will parse (matches the sample's MaxJsonLength).
constexpr qint64 kMaxJsonBytes = 1024 * 1024;

// BR_* transport return codes (BarcodeReaderTrigger.h). BR_OK is transport
// success only; the JSON's ok/code/state decides the business outcome.
constexpr int kBrOk = 0;
constexpr int kBrNotConnected = 2;
constexpr int kBrTimeout = 3;
constexpr int kBrBufferTooSmall = 5;
constexpr int kBrResponseTooLarge = 6;

// The SDK requires 1..64 ASCII letters/digits/_/- (README_CN.md:105). This
// shape is unique per cycle without pulling in a UUID dependency.
QString makeRequestId(quint64 sequence)
{
    return QStringLiteral("job-%1-%2")
        .arg(QDateTime::currentDateTime().toString(
            QStringLiteral("yyyyMMdd-hhmmsszzz")))
        .arg(sequence);
}

QString businessCodeReason(const QString &code)
{
    if (code == QLatin1String("no_project"))
        return QStringLiteral("扫码程序未打开项目");
    if (code == QLatin1String("wrong_mode"))
        return QStringLiteral("扫码程序未处于「触发 → 第三方 APP」模式");
    if (code == QLatin1String("not_running"))
        return QStringLiteral("扫码程序未运行，请点击运行");
    if (code == QLatin1String("no_rects"))
        return QStringLiteral("扫码项目未配置识别区域（RECT）");
    if (code == QLatin1String("not_ready"))
        return QStringLiteral("相机尚无可用画面");
    if (code == QLatin1String("busy"))
        return QStringLiteral("上一轮解码尚未完成");
    if (code == QLatin1String("not_found"))
        return QStringLiteral("本轮结果已不在扫码程序缓存中");
    if (code.isEmpty())
        return QStringLiteral("扫码失败");
    return QStringLiteral("扫码失败（%1）").arg(code);
}

// Parses an SDK reply into a row list. Returns false with `reason` filled when
// the payload is unusable.
bool parseRows(const QByteArray &json, QVector<BarcodeRow> *rows, QString *reason)
{
    if (json.size() > kMaxJsonBytes) {
        *reason = QStringLiteral("扫码结果过大，请检查相机/识别区域配置");
        return false;
    }
    QJsonParseError parseError{};
    const QJsonObject object =
        QJsonDocument::fromJson(json, &parseError).object();
    if (parseError.error != QJsonParseError::NoError) {
        *reason = QStringLiteral("扫码结果解析失败");
        return false;
    }
    const QJsonArray array = object.value(QStringLiteral("rows")).toArray();
    for (const QJsonValue &value : array) {
        const QJsonObject row = value.toObject();
        BarcodeRow parsed;
        parsed.barcode = row.value(QStringLiteral("barcode")).toString();
        parsed.format = row.value(QStringLiteral("format")).toString();
        parsed.cameraId = row.value(QStringLiteral("cameraId")).toString();
        parsed.rectId = row.value(QStringLiteral("rectId")).toInt();
        parsed.sequence = row.value(QStringLiteral("sequence")).toInt();
        rows->append(parsed);
    }
    return true;
}

} // namespace

#ifdef _WIN32

namespace {

using StatusFn = int (*)(const wchar_t *, char *, unsigned int,
                         unsigned int *, unsigned int);
using IdFn = int (*)(const wchar_t *, const char *, char *, unsigned int,
                     unsigned int *, unsigned int);

// Production façade over the real DLL (README_CN.md:131 sanctions
// LoadLibraryW/GetProcAddress; the exports are undecorated __cdecl).
//
// The DLL is loaded BY NAME so Windows searches the running executable's own
// directory first — deployment is "put BarcodeReaderTrigger.dll next to
// hlm_app.exe". A missing DLL (or a wrong-bitness one, which fails the export
// lookup) is a LibraryUnavailable reply, never silence.
class WindowsBarcodeSdk : public IBarcodeSdk
{
public:
    WindowsBarcodeSdk()
    {
        m_module = ::LoadLibraryW(L"BarcodeReaderTrigger.dll");
        if (m_module == nullptr)
            return; // LibraryUnavailable for every call
        m_status = reinterpret_cast<StatusFn>(
            ::GetProcAddress(m_module, "BR_GetStatusW"));
        m_trigger = reinterpret_cast<IdFn>(
            ::GetProcAddress(m_module, "BR_TriggerW"));
        m_result = reinterpret_cast<IdFn>(
            ::GetProcAddress(m_module, "BR_GetResultW"));
        if (m_status == nullptr || m_trigger == nullptr || m_result == nullptr) {
            ::FreeLibrary(m_module);
            m_module = nullptr;
        }
    }

    ~WindowsBarcodeSdk() override
    {
        if (m_module != nullptr)
            ::FreeLibrary(m_module);
    }

    BarcodeSdkReply status() override
    {
        if (m_status == nullptr)
            return reply(BarcodeSdkStatus::LibraryUnavailable, 0, nullptr);
        QByteArray buffer(kResponseCapacity, '\0');
        unsigned int required = 0;
        const int rc = m_status(nullptr, buffer.data(),
                                static_cast<unsigned int>(buffer.size()),
                                &required, kCallTimeoutMs);
        return reply(static_cast<BarcodeSdkStatus>(rc), rc, &buffer);
    }

    BarcodeSdkReply trigger(const QString &requestId) override
    {
        return callWithId(m_trigger, requestId);
    }
    BarcodeSdkReply result(const QString &requestId) override
    {
        return callWithId(m_result, requestId);
    }

private:
    // requiredBytes includes the NUL (README_CN.md:28), so the payload is
    // requiredBytes - 1 bytes. Grows and retries once on BR_BUFFER_TOO_SMALL —
    // the caller is expected to use the reported size.
    static BarcodeSdkReply reply(BarcodeSdkStatus status, int rc,
                                 const QByteArray *buffer)
    {
        BarcodeSdkReply out;
        out.status = status;
        if (rc == kBrOk && buffer != nullptr)
            out.json = *buffer;
        return out;
    }

    BarcodeSdkReply callWithId(IdFn fn, const QString &requestId)
    {
        if (fn == nullptr)
            return reply(BarcodeSdkStatus::LibraryUnavailable, 0, nullptr);
        const QByteArray id = requestId.toUtf8();
        QByteArray buffer(kResponseCapacity, '\0');
        for (int attempt = 0; attempt < 2; ++attempt) {
            unsigned int required = 0;
            const int rc = fn(nullptr, id.constData(), buffer.data(),
                              static_cast<unsigned int>(buffer.size()),
                              &required, kCallTimeoutMs);
            if (rc == kBrOk) {
                if (required > 0)
                    out_json = buffer;
                return reply(BarcodeSdkStatus::Ok, rc, &buffer);
            }
            if (rc == kBrBufferTooSmall && attempt == 0 && required > 0) {
                buffer = QByteArray(static_cast<int>(required), '\0');
                continue;
            }
            return reply(static_cast<BarcodeSdkStatus>(rc), rc, nullptr);
        }
        return reply(BarcodeSdkStatus::BufferTooSmall, kBrBufferTooSmall, nullptr);
    }

    QByteArray out_json;
    HMODULE m_module = nullptr;
    StatusFn m_status = nullptr;
    IdFn m_trigger = nullptr;
    IdFn m_result = nullptr;
};

} // namespace

std::unique_ptr<IBarcodeSdk> makeSystemBarcodeSdk()
{
    return std::make_unique<WindowsBarcodeSdk>();
}

#else // !_WIN32

// Linux dev loop: the SDK is a Windows named-pipe DLL, so every call reports
// the same "not available" status. Every test injects a fake instead, so this
// only ever shows up when someone runs the app on a non-Windows host — where
// the operator-facing text still tells them to start the scan program.
std::unique_ptr<IBarcodeSdk> makeSystemBarcodeSdk()
{
    class UnavailableBarcodeSdk : public IBarcodeSdk
    {
    public:
        BarcodeSdkReply status() override
        {
            return {BarcodeSdkStatus::LibraryUnavailable, QByteArray()};
        }
        BarcodeSdkReply trigger(const QString &) override
        {
            return {BarcodeSdkStatus::LibraryUnavailable, QByteArray()};
        }
        BarcodeSdkReply result(const QString &) override
        {
            return {BarcodeSdkStatus::LibraryUnavailable, QByteArray()};
        }
    };
    return std::make_unique<UnavailableBarcodeSdk>();
}

#endif

BarcodeReaderSdkSource::BarcodeReaderSdkSource(IBarcodeSdk *sdk, QObject *parent)
    : IBarcodeSource(parent)
    , m_ownerThread(QThread::currentThread())
{
    if (sdk != nullptr) {
        // Caller-owned: never deleted and never reparented (the same rule as
        // AppConfig::plcGateway / serialPortDiscovery).
        m_sdk = sdk;
    } else {
        m_ownedSdk = makeSystemBarcodeSdk();
        m_sdk = m_ownedSdk.get();
    }
}

BarcodeReaderSdkSource::~BarcodeReaderSdkSource()
{
    stop();
    delete m_deadline;
    m_deadline = nullptr;
}

void BarcodeReaderSdkSource::start()
{
    if (m_thread)
        return;
    m_thread = new QThread;
    // The blocking SDK calls and the file append run on this thread (contract
    // forbidden_change: no scanner SDK calls or blocking waits in QWidget code
    // or on the UI thread).
    moveToThread(m_thread);
    connect(m_thread, &QThread::finished, this,
            [this]() { moveToThread(m_ownerThread); }, Qt::DirectConnection);
    m_thread->start();
}

void BarcodeReaderSdkSource::stop()
{
    if (!m_thread)
        return;
    QThread *worker = m_thread;
    worker->quit();
    worker->wait();
    m_thread = nullptr;
    delete worker;

    // The worker's timers died with its event loop; drop the flag so a restart
    // is not blocked by a cycle that can no longer finish.
    QMutexLocker lock(&m_mutex);
    m_cycleInProgress = false;
}

void BarcodeReaderSdkSource::setResultPath(const QString &path)
{
    QMutexLocker lock(&m_mutex);
    m_path = path;
}

QString BarcodeReaderSdkSource::resultPath() const
{
    QMutexLocker lock(&m_mutex);
    return m_path;
}

bool BarcodeReaderSdkSource::cycleInProgress() const
{
    QMutexLocker lock(&m_mutex);
    return m_cycleInProgress;
}

bool BarcodeReaderSdkSource::requestRead()
{
    {
        QMutexLocker lock(&m_mutex);
        if (m_cycleInProgress)
            return false; // never queued; the caller surfaces the overlap
        m_cycleInProgress = true;
    }
    QMetaObject::invokeMethod(this, &BarcodeReaderSdkSource::performCycle,
                              Qt::QueuedConnection);
    return true;
}

void BarcodeReaderSdkSource::performCycle()
{
    BarcodeResult result;
    {
        QMutexLocker lock(&m_mutex);
        result.sequence = ++m_sequence;
    }
    result.readAt = QDateTime::currentDateTime();

    const QString path = resultPath().trimmed();
    if (path.isEmpty()) {
        // Visibly 未配置: nothing is triggered, and the surface says so.
        result.state = BarcodeState::NotConfigured;
        emitTerminal(result);
        return;
    }

    // serverId identifies this run of the scan program. Remembering it lets a
    // restart mid-cycle be detected instead of trusting a result produced by a
    // different server instance (README_CN.md:108).
    const BarcodeSdkReply status = m_sdk->status();
    if (status.status != BarcodeSdkStatus::Ok) {
        result.state = BarcodeState::Failed;
        result.detail = transportReason(status.status);
        emitTerminal(result);
        return;
    }
    QJsonParseError statusError{};
    const QJsonObject statusObject =
        QJsonDocument::fromJson(status.json, &statusError).object();
    if (statusError.error == QJsonParseError::NoError)
        m_serverId = statusObject.value(QStringLiteral("serverId")).toString();

    m_requestId = makeRequestId(result.sequence);
    result.requestId = m_requestId;

    const BarcodeSdkReply triggered = m_sdk->trigger(m_requestId);
    bool keepPolling = false;
    if (applyReply(triggered, &result, &keepPolling)) {
        persistRows(&result);
        emitTerminal(result);
        return;
    }
    if (!keepPolling) {
        persistRows(&result);
        emitTerminal(result);
        return;
    }

    if (m_deadline == nullptr)
        m_deadline = new QElapsedTimer;
    m_deadline->start();
    if (m_poll == nullptr) {
        m_poll = new QTimer(this);
        m_poll->setInterval(kPollIntervalMs);
        connect(m_poll, &QTimer::timeout, this,
                &BarcodeReaderSdkSource::onPollTimeout);
    }
    m_poll->start();
}

void BarcodeReaderSdkSource::onPollTimeout()
{
    if (m_deadline != nullptr && m_deadline->elapsed() >= kCycleDeadlineMs) {
        if (m_poll != nullptr)
            m_poll->stop();
        BarcodeResult result;
        {
            QMutexLocker lock(&m_mutex);
            result.sequence = m_sequence;
        }
        result.readAt = QDateTime::currentDateTime();
        result.requestId = m_requestId;
        // Giving up on the wait does NOT cancel the accepted decode; the
        // result stays queryable under the same requestId. The operator sees a
        // visible timeout instead of an open-ended pending state.
        result.state = BarcodeState::Failed;
        result.detail =
            QStringLiteral("扫码结果等待超时（%1 秒）").arg(kCycleDeadlineMs / 1000);
        emitTerminal(result);
        return;
    }

    BarcodeResult result;
    {
        QMutexLocker lock(&m_mutex);
        result.sequence = m_sequence;
    }
    result.readAt = QDateTime::currentDateTime();
    result.requestId = m_requestId;

    bool keepPolling = false;
    if (applyReply(m_sdk->result(m_requestId), &result, &keepPolling)) {
        if (m_poll != nullptr)
            m_poll->stop();
        persistRows(&result);
        emitTerminal(result);
        return;
    }
    if (!keepPolling) {
        if (m_poll != nullptr)
            m_poll->stop();
        persistRows(&result);
        emitTerminal(result);
        return;
    }
    // Otherwise keep the timer running: pending / busy / a retryable transport
    // error, always with the SAME requestId (never a new one).
}

bool BarcodeReaderSdkSource::applyReply(const BarcodeSdkReply &reply,
                                        BarcodeResult *result, bool *keepPolling)
{
    *keepPolling = false;

    if (reply.status != BarcodeSdkStatus::Ok) {
        if (reply.status == BarcodeSdkStatus::Timeout) {
            // A transport timeout does not cancel an accepted job, so keep
            // querying the SAME requestId until the cycle deadline.
            *keepPolling = true;
            return false;
        }
        result->state = BarcodeState::Failed;
        result->detail = transportReason(reply.status);
        return true;
    }

    QJsonParseError parseError{};
    const QJsonObject object =
        QJsonDocument::fromJson(reply.json, &parseError).object();
    if (parseError.error != QJsonParseError::NoError) {
        result->state = BarcodeState::Failed;
        result->detail = QStringLiteral("扫码结果解析失败");
        return true;
    }
    if (!object.value(QStringLiteral("ok")).toBool()) {
        const QString code = object.value(QStringLiteral("code")).toString();
        // busy: the previous round is still decoding (the SDK does not queue a
        // second trigger). not_ready: no decodable frame yet. Both are
        // transient — the cycle keeps querying the SAME requestId, and the
        // cycle deadline is what finally converges a permanently busy server.
        if (code == QLatin1String("busy") || code == QLatin1String("not_ready")) {
            *keepPolling = true;
            return false;
        }
        result->state = BarcodeState::Failed;
        result->detail = businessCodeReason(code);
        return true;
    }

    // The server restarted: the accepted request may or may not have run, and
    // nothing may be assumed (README_CN.md:108).
    const QString serverId =
        object.value(QStringLiteral("serverId")).toString();
    if (!m_serverId.isEmpty() && !serverId.isEmpty() && serverId != m_serverId) {
        result->state = BarcodeState::Failed;
        result->detail =
            QStringLiteral("扫码服务已重启，本轮结果无法确认，请重新扫码");
        return true;
    }

    const QString state = object.value(QStringLiteral("state")).toString();
    if (state == QLatin1String("pending")) {
        *keepPolling = true;
        return false;
    }
    if (state != QLatin1String("completed")) {
        // state == "failed" (or unknown). The vendor sample would keep polling
        // until its deadline; this adapter converges at once, because every
        // accepted operation must reach a visible terminal state.
        result->state = BarcodeState::Failed;
        const QString message =
            object.value(QStringLiteral("message")).toString();
        result->detail =
            message.isEmpty()
                ? businessCodeReason(
                      object.value(QStringLiteral("code")).toString())
                : message;
        return true;
    }

    QString parseFailure;
    if (!parseRows(reply.json, &result->rows, &parseFailure)) {
        result->state = BarcodeState::Failed;
        result->detail = parseFailure;
        return true;
    }
    for (const BarcodeRow &row : result->rows) {
        if (row.barcode.trimmed().isEmpty())
            ++result->emptyPositions;
        else
            ++result->decodedCount;
    }
    if (result->decodedCount == 0) {
        // A real outcome, not a failure: the SDK looked and decoded nothing.
        result->state = BarcodeState::NoCode;
        result->detail = QStringLiteral("本轮未识别到条码");
        return true;
    }
    // The first decoded barcode drives the single-line surface and its fields.
    for (const BarcodeRow &row : result->rows) {
        if (row.barcode.trimmed().isEmpty())
            continue;
        result->line = row.barcode;
        result->fields = result->line.split(QLatin1Char('^'));
        for (QString &field : result->fields)
            field = field.trimmed();
        break;
    }
    result->state = BarcodeState::Ok;
    return true;
}

void BarcodeReaderSdkSource::persistRows(BarcodeResult *result)
{
    if (result->decodedCount == 0)
        return; // nothing decoded: the file is left exactly as it was

    const QString path = resultPath().trimmed();
    if (path.isEmpty()) {
        result->persistDetail = QStringLiteral("未配置存储路径");
        return;
    }
    // The parent directory must already exist: creating it silently would hide
    // a mistyped path behind an empty folder the operator never asked for.
    const QFileInfo info(path);
    if (!info.dir().exists()) {
        result->persistDetail = QStringLiteral("存储目录不存在");
        return;
    }

    QFile file(path);
    if (!file.open(QIODevice::Append)) {
        result->persistDetail = QStringLiteral("结果文件无法写入（%1）")
                                    .arg(file.errorString());
        return;
    }
    // One line per decoded barcode, in table order, CRLF-terminated —
    // byte-for-byte the format the scan program itself writes
    // (需求/扫码相关/Barcode.txt). Empty positions are NOT written: they are
    // not barcodes.
    for (const BarcodeRow &row : result->rows) {
        if (row.barcode.trimmed().isEmpty())
            continue;
        file.write(row.barcode.toUtf8());
        file.write("\r\n");
    }
    file.close();
    result->persisted = true;
}

void BarcodeReaderSdkSource::emitTerminal(const BarcodeResult &result)
{
    if (m_poll != nullptr)
        m_poll->stop();
    {
        QMutexLocker lock(&m_mutex);
        m_cycleInProgress = false;
    }
    // Clearing the flag BEFORE the signal lets a caller that immediately
    // submits the next cycle from the same slot succeed.
    emit resultReady(result);
}

QString BarcodeReaderSdkSource::transportReason(BarcodeSdkStatus status)
{
    switch (status) {
    case BarcodeSdkStatus::NotConnected:
    case BarcodeSdkStatus::LibraryUnavailable:
        return QStringLiteral("扫码服务未启动，请先打开 BarcodeReader 并点击运行");
    case BarcodeSdkStatus::BufferTooSmall:
    case BarcodeSdkStatus::ResponseTooLarge:
        return QStringLiteral("扫码结果过大，请检查相机/识别区域配置");
    case BarcodeSdkStatus::InvalidArgument:
        return QStringLiteral("扫码调用参数无效");
    case BarcodeSdkStatus::IoError:
        return QStringLiteral("扫码通讯错误，请检查扫码程序");
    case BarcodeSdkStatus::Timeout:
        return QStringLiteral("扫码通讯超时");
    case BarcodeSdkStatus::Ok:
        break;
    }
    return QStringLiteral("扫码失败");
}

} // namespace hlm
