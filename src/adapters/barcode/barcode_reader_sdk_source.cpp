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
#include <QProcess>
#include <QTimer>
#include <QUuid>

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
// Largest response this adapter will parse (matches the sample's MaxJsonLength).
constexpr qint64 kMaxJsonBytes = 1024 * 1024;

// BR_* transport return codes this adapter reasons about (BarcodeReaderTrigger.h).
// BR_OK is transport success only; the JSON's ok/code/state decides the business
// outcome. The other codes only travel through as BarcodeSdkStatus.
constexpr int kBrOk = 0;
constexpr int kBrBufferTooSmall = 5;

// The SDK requires 1..64 ASCII letters/digits/_/- (README_CN.md:105), and the
// vendor recommends a GUID: the server dedups repeats within its 64-entry
// cache, so an id reused across a restart would be answered from the previous
// run's result. A UUID makes that impossible rather than merely unlikely.
QString makeRequestId()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces).remove(QLatin1Char('-'));
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

QByteArray barcodePayloadFromBuffer(const QByteArray &raw, quint32 requiredBytes)
{
    const qsizetype cap = qMin<qsizetype>(
        raw.size(),
        requiredBytes > 0 ? static_cast<qsizetype>(requiredBytes) - 1 : raw.size());
    // An oversized/unset requiredBytes (or a buffer the SDK left unpadded): read
    // up to the first NUL rather than trusting the count blindly.
    const qsizetype nul = raw.indexOf('\0');
    const qsizetype length = nul >= 0 ? qMin(cap, nul) : cap;
    return raw.left(length);
}

bool barcodeForwardArgument(const QVector<BarcodeRow> &rows, QString *argument)
{
    QStringList decoded;
    for (const BarcodeRow &row : rows) {
        const QString code = row.barcode.trimmed();
        if (code.isEmpty())
            continue; // an empty position is not a barcode
        // One space is the separator, so a barcode that itself contains one
        // cannot be told apart from two barcodes downstream. Refusing is
        // visible; sending it would corrupt the data silently.
        if (code.contains(QLatin1Char(' ')))
            return false;
        decoded.append(code);
    }
    *argument = decoded.join(QLatin1Char(' '));
    return true;
}

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
    explicit WindowsBarcodeSdk(const QString &dllPath)
    {
        // Empty → load BY NAME, so Windows searches the running executable's
        // own directory first ("put BarcodeReaderTrigger.dll next to
        // hlm_app.exe"). Non-empty → load exactly that file, because the
        // operator pointed at a DLL kept elsewhere (user decision 2026-09-23).
        // A missing DLL (or a wrong-bitness one, which fails the export lookup)
        // is a LibraryUnavailable reply, never silence.
        const QString native = QDir::toNativeSeparators(dllPath.trimmed());
        m_module = native.isEmpty()
            ? ::LoadLibraryW(L"BarcodeReaderTrigger.dll")
            : ::LoadLibraryW(reinterpret_cast<const wchar_t *>(native.utf16()));
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
            return unavailable();
        QByteArray buffer(kResponseCapacity, '\0');
        unsigned int required = 0;
        const int rc = m_status(nullptr, buffer.data(),
                                static_cast<unsigned int>(buffer.size()),
                                &required, kCallTimeoutMs);
        return replyFrom(static_cast<BarcodeSdkStatus>(rc), buffer, required);
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
    static BarcodeSdkReply unavailable()
    {
        BarcodeSdkReply out;
        out.status = BarcodeSdkStatus::LibraryUnavailable;
        return out;
    }

    // A transport failure carries no payload; a success carries exactly the
    // payload the SDK reported, never the whole NUL-padded buffer.
    static BarcodeSdkReply replyFrom(BarcodeSdkStatus status,
                                     const QByteArray &buffer, unsigned int required)
    {
        BarcodeSdkReply out;
        out.status = status;
        if (status == BarcodeSdkStatus::Ok)
            out.json = barcodePayloadFromBuffer(buffer, required);
        return out;
    }

    BarcodeSdkReply callWithId(IdFn fn, const QString &requestId)
    {
        if (fn == nullptr)
            return unavailable();
        const QByteArray id = requestId.toUtf8();
        QByteArray buffer(kResponseCapacity, '\0');
        for (int attempt = 0; attempt < 2; ++attempt) {
            unsigned int required = 0;
            const int rc = fn(nullptr, id.constData(), buffer.data(),
                              static_cast<unsigned int>(buffer.size()),
                              &required, kCallTimeoutMs);
            if (rc == kBrOk) {
                return replyFrom(BarcodeSdkStatus::Ok, buffer, required);
            }
            if (rc == kBrBufferTooSmall && attempt == 0 && required > 0) {
                // The reported size is the caller's grow-and-retry signal.
                buffer = QByteArray(static_cast<int>(required), '\0');
                continue;
            }
            BarcodeSdkReply out;
            out.status = static_cast<BarcodeSdkStatus>(rc);
            return out;
        }
        BarcodeSdkReply out;
        out.status = BarcodeSdkStatus::BufferTooSmall;
        return out;
    }

    HMODULE m_module = nullptr;
    StatusFn m_status = nullptr;
    IdFn m_trigger = nullptr;
    IdFn m_result = nullptr;
};

} // namespace

std::unique_ptr<IBarcodeSdk> makeSystemBarcodeSdk(const QString &dllPath)
{
    return std::make_unique<WindowsBarcodeSdk>(dllPath);
}

#else // !_WIN32

// Linux dev loop: the SDK is a Windows named-pipe DLL, so every call reports
// the same "not available" status. Every test injects a fake instead, so this
// only ever shows up when someone runs the app on a non-Windows host — where
// the operator-facing text still tells them to start the scan program.
std::unique_ptr<IBarcodeSdk> makeSystemBarcodeSdk(const QString &)
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

BarcodeReaderSdkSource::BarcodeReaderSdkSource(IBarcodeSdk *sdk, Config config,
                                               QObject *parent)
    : IBarcodeSource(parent)
    , m_config(config)
    , m_ownerThread(QThread::currentThread())
{
    if (sdk != nullptr) {
        // Caller-owned: never deleted and never reparented (the same rule as
        // AppConfig::plcGateway / serialPortDiscovery).
        m_sdk = sdk;
    } else {
        m_ownedSdk = makeSystemBarcodeSdk(QString());
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

void BarcodeReaderSdkSource::setDllPath(const QString &path)
{
    QMutexLocker lock(&m_mutex);
    const QString trimmed = path.trimmed();
    if (trimmed == m_dllPath)
        return;
    m_dllPath = trimmed;
    // Only a flag here: the module itself is loaded and freed on the worker
    // thread, at the start of the next cycle (applyPendingDllPath). Doing it
    // from this (caller) thread could free the library under an in-flight call.
    m_sdkPathDirty = true;
}

QString BarcodeReaderSdkSource::dllPath() const
{
    QMutexLocker lock(&m_mutex);
    return m_dllPath;
}

void BarcodeReaderSdkSource::setForwardExePath(const QString &path)
{
    QMutexLocker lock(&m_mutex);
    m_forwardPath = path.trimmed();
}

QString BarcodeReaderSdkSource::forwardExePath() const
{
    QMutexLocker lock(&m_mutex);
    return m_forwardPath;
}

void BarcodeReaderSdkSource::applyPendingDllPath()
{
    QString path;
    {
        QMutexLocker lock(&m_mutex);
        if (!m_sdkPathDirty)
            return;
        m_sdkPathDirty = false;
        path = m_dllPath;
    }
    if (m_ownedSdk == nullptr) {
        // An injected SDK is caller-owned and knows nothing about paths; the
        // setting is still recorded above so the UI round-trip stays honest.
        return;
    }
    // The old module is freed by this assignment, on the worker thread, between
    // cycles — never while one of its calls is in flight. The cached serverId
    // is NOT cleared: if the new library really is a different server instance,
    // the next reply's serverId differs and the cycle converges as "扫码服务已
    // 重启，本轮结果无法确认" — which is exactly the honest outcome. Clearing it
    // would disable that check and let the old server's result through.
    m_ownedSdk = makeSystemBarcodeSdk(path);
    m_sdk = m_ownedSdk.get();
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
    // A DLL-path change takes effect here, on the worker thread, between
    // cycles: never while a call into the previous module is in flight.
    applyPendingDllPath();

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
        finishCycle(&result);
        return;
    }

    // serverId identifies this run of the scan program. Remembering it lets a
    // restart mid-cycle be detected instead of trusting a result produced by a
    // different server instance (README_CN.md:108).
    const BarcodeSdkReply status = m_sdk->status();
    if (status.status != BarcodeSdkStatus::Ok) {
        result.state = BarcodeState::Failed;
        result.detail = transportReason(status.status);
        finishCycle(&result);
        return;
    }
    QJsonParseError statusError{};
    const QJsonObject statusObject =
        QJsonDocument::fromJson(status.json, &statusError).object();
    if (statusError.error == QJsonParseError::NoError)
        m_serverId = statusObject.value(QStringLiteral("serverId")).toString();

    m_requestId = makeRequestId();
    result.requestId = m_requestId;

    const BarcodeSdkReply triggered = m_sdk->trigger(m_requestId);
    bool keepPolling = false;
    if (applyReply(triggered, &result, &keepPolling, /*isTriggerReply=*/true)) {
        finishCycle(&result);
        return;
    }
    if (!keepPolling) {
        finishCycle(&result);
        return;
    }

    if (m_deadline == nullptr)
        m_deadline = new QElapsedTimer;
    m_deadline->start();
    if (m_poll == nullptr) {
        m_poll = new QTimer(this);
        m_poll->setInterval(m_config.pollIntervalMs);
        connect(m_poll, &QTimer::timeout, this,
                &BarcodeReaderSdkSource::onPollTimeout);
    }
    m_poll->start();
}

void BarcodeReaderSdkSource::onPollTimeout()
{
    if (m_deadline != nullptr && m_deadline->elapsed() >= m_config.cycleDeadlineMs) {
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
            QStringLiteral("扫码结果等待超时（%1 秒）")
                .arg(m_config.cycleDeadlineMs / 1000);
        finishCycle(&result);
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
    if (applyReply(m_sdk->result(m_requestId), &result, &keepPolling,
                   /*isTriggerReply=*/false)) {
        if (m_poll != nullptr)
            m_poll->stop();
        finishCycle(&result);
        return;
    }
    if (!keepPolling) {
        if (m_poll != nullptr)
            m_poll->stop();
        finishCycle(&result);
        return;
    }
    // Otherwise keep the timer running: pending / busy / a retryable transport
    // error, always with the SAME requestId (never a new one).
}

bool BarcodeReaderSdkSource::applyReply(const BarcodeSdkReply &reply,
                                        BarcodeResult *result, bool *keepPolling,
                                        bool isTriggerReply)
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
        // busy: the previous round is still decoding. not_ready: no decodable
        // frame yet. Both are TRANSIENT ONLY when they answer a result query —
        // a poll that comes back busy simply means "not finished". In a TRIGGER
        // reply they are terminal for this attempt, because the SDK does not
        // queue triggers: nothing was accepted under this requestId, so polling
        // it can only ever return not_found. Converging at once is what keeps
        // the operator from waiting out the whole deadline for an answer the
        // adapter already has.
        const bool transient = !isTriggerReply
            && (code == QLatin1String("busy") || code == QLatin1String("not_ready"));
        if (transient) {
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

    // A reply whose rows were omitted because the result exceeded the SDK's
    // 128 KiB payload limit. The README is explicit that a truncated or empty
    // row list must NOT be read as "decoded nothing" — presenting this as
    // NoCode would tell the operator "no barcode this cycle" for a cycle that
    // decoded six. It is a failure of the transfer, not a result.
    if (object.value(QStringLiteral("rowsTruncated")).toBool()) {
        result->state = BarcodeState::Failed;
        result->detail = QStringLiteral("扫码结果过大被截断，请按相机/识别区域筛选后重试");
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
    const qint64 startSize = file.size();
    // One line per decoded barcode, in table order, CRLF-terminated —
    // byte-for-byte the format the scan program itself writes
    // (需求/扫码相关/Barcode.txt). Empty positions are NOT written: they are
    // not barcodes.
    QByteArray payload;
    for (const BarcodeRow &row : result->rows) {
        if (row.barcode.trimmed().isEmpty())
            continue;
        payload += row.barcode.toUtf8();
        payload += "\r\n";
    }
    file.write(payload);
    // A short write (a full disk, a quota, a device that filled up mid-batch)
    // leaves a traceability file quietly missing rows. On this file a silent
    // partial batch is worse than a visible failure, so report it: the operator
    // sees that the barcode was read but not completely stored.
    const bool shortWrite = file.error() != QFileDevice::NoError
                            || file.size() - startSize != payload.size();
    file.close();
    if (shortWrite) {
        result->persistDetail = QStringLiteral("结果文件写入不完整，请检查磁盘空间");
        return;
    }
    result->persisted = true;
}

// Runs the configured program once, with this cycle's barcodes as ONE
// space-joined argument (user decision 2026-09-23). Everything here is
// non-blocking except the start and the bounded wait, and both run on the
// worker thread — never on the UI thread (contract forbidden_change).
void BarcodeReaderSdkSource::forwardRows(BarcodeResult *result)
{
    if (result->decodedCount == 0)
        return; // nothing decoded: there is nothing to hand downstream

    const QString program = forwardExePath().trimmed();
    if (program.isEmpty())
        return; // not configured: neither field claims anything happened

    QString argument;
    if (!barcodeForwardArgument(result->rows, &argument)) {
        result->forwardDetail =
            QStringLiteral("条码含空格，按空格拼接会串位，本轮未外发");
        return;
    }
    if (argument.isEmpty()) {
        result->forwardDetail = QStringLiteral("本轮无可外发条码");
        return;
    }
    // Check the path ourselves: QProcess would report this as a generic
    // "failed to start", which is not something the operator can act on.
    if (!QFileInfo::exists(program)) {
        result->forwardDetail =
            QStringLiteral("外发程序不存在：%1").arg(program);
        return;
    }

    QProcess process;
#ifdef _WIN32
    // Without this every board flashes a console window over the HMI.
    process.setCreateProcessArgumentsModifier(
        [](QProcess::CreateProcessArguments *args) {
            args->flags |= CREATE_NO_WINDOW;
        });
#endif
    // Program and argument are passed separately: no shell is involved, so a
    // program path containing spaces still works.
    process.start(program, QStringList{argument});
    if (!process.waitForStarted(2000)) {
        result->forwardDetail =
            QStringLiteral("外发程序无法启动（%1）").arg(process.errorString());
        return;
    }
    if (!process.waitForFinished(m_config.forwardTimeoutMs)) {
        // A hung program must not hold the cycle open; kill and report.
        process.kill();
        process.waitForFinished(1000);
        result->forwardDetail =
            QStringLiteral("外发超时（%1 秒）").arg(m_config.forwardTimeoutMs / 1000);
        return;
    }
    if (process.exitStatus() != QProcess::NormalExit) {
        result->forwardDetail = QStringLiteral("外发程序异常终止");
        return;
    }
    if (process.exitCode() != 0) {
        result->forwardDetail =
            QStringLiteral("外发程序返回 %1").arg(process.exitCode());
        return;
    }
    result->forwarded = true;
}

// The single convergence path for a terminal cycle: file append, then forward,
// then exactly one terminal result. Keeping the three steps in one place is
// what stops a future converge site from silently skipping the side effects.
void BarcodeReaderSdkSource::finishCycle(BarcodeResult *result)
{
    persistRows(result);
    forwardRows(result);
    emitTerminal(*result);
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
