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

// Largest response this adapter will parse (matches the sample's MaxJsonLength).
constexpr qint64 kMaxJsonBytes = 1024 * 1024;
// How long one CLI invocation may take before it is killed and reported as a
// transport timeout. The vendor's own examples pass 2000 ms to the DLL and the
// CLI inherits that budget; the extra second covers process start-up on a busy
// shop-floor machine.
constexpr qint64 kCallTimeoutMs = 3000;

// BR_* transport return codes this adapter reasons about (BarcodeReaderTrigger.h).
// BR_OK is transport success only; the JSON's ok/code/state decides the business
// outcome. The other codes only travel through as BarcodeSdkStatus, and the
// whole range is what the CLI can legally exit with.
constexpr int kBrOk = 0;
constexpr int kBrResponseTooLarge = 6;

// A unique request id for one scan cycle. The SDK accepts 1..64 ASCII
// letters/digits/_/- (README_CN.md:105) and the server dedups repeats within its
// 64-entry cache, so the id must not be reused across cycles OR across runs.
//
// USER DECISION 2026-09-23: this is a TIMESTAMP — `yyyyMMddHHmmsszzz`
// (millisecond precision, e.g. 20260923103040123). The operator asked for a
// timestamp rather than an opaque UUID because it is readable: the same id shows
// up in the scanner program's own logs, so a cycle can be traced by eye.
//
// The millisecond field is load-bearing, not decoration. A cycle can finish in
// ~2 ms (the observed bench timing) while a second-triggered cycle would land in
// the same second; two cycles sharing an id would make the server answer the
// second trigger from its cache with the FIRST board's barcodes. Milliseconds
// make that a 1-in-1000 collision instead of a 1-in-1 one, and the sequence
// suffix below removes it entirely for two cycles inside the SAME millisecond.
QString makeRequestId(quint64 sequence)
{
    const QString stamp =
        QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMddHHmmsszzz"));
    return QStringLiteral("%1-%2").arg(stamp).arg(sequence);
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

bool barcodeForwardArguments(const QVector<BarcodeRow> &rows,
                             QStringList *arguments)
{
    arguments->clear();
    for (const BarcodeRow &row : rows) {
        const QString code = row.barcode.trimmed();
        if (code.isEmpty())
            continue; // an empty position is not a barcode
        // TAB separates the downstream program's frame fields and CR/LF
        // terminate them, so a barcode carrying either cannot be represented.
        // Refusing is visible; sending it would corrupt the frame silently.
        if (code.contains(QLatin1Char('\t')) || code.contains(QLatin1Char('\r'))
            || code.contains(QLatin1Char('\n')))
            return false;
        arguments->append(code);
    }
    return true;
}

// Production transport: the vendor's own CLI, run as a child process
// (user decision 2026-09-23).
//
// WHY NOT THE DLL: loading BarcodeReaderTrigger.dll in-process failed on the
// operator's machine for reasons the HMI could not diagnose, while
// `trigger_client.exe` — the vendor's own reference client, built from
// example_c.c — is the exact command that machine had already run by hand. So
// the HMI runs what the operator runs and reads its stdout. Same code on every
// platform, no FreeLibrary/reload machinery, and the program the operator can
// test from a cmd prompt is the program the HMI drives.
//
// The CLI's shape (README_CN.md:134-138, example_c.c):
//   trigger_client.exe <endpoint> status|barcodes|trigger|result [requestId]
// It prints the UTF-8 JSON response on stdout and returns the BR_* transport
// code as its exit status — so a non-zero exit IS the transport failure, and the
// two error channels do not overlap. The endpoint may be the empty string, which
// selects the default BarcodeReader.Trigger.v1 (BarcodeReaderTrigger.h:26), so
// the operator never has to know that name.
class BarcodeCliClient : public IBarcodeSdk
{
public:
    BarcodeCliClient(QString program, qint64 callTimeoutMs)
        : m_program(std::move(program))
        , m_callTimeoutMs(callTimeoutMs)
    {
    }

    BarcodeSdkReply status() override
    {
        return run(QStringLiteral("status"), QString());
    }
    BarcodeSdkReply trigger(const QString &requestId) override
    {
        return run(QStringLiteral("trigger"), requestId);
    }
    BarcodeSdkReply result(const QString &requestId) override
    {
        return run(QStringLiteral("result"), requestId);
    }

private:
    BarcodeSdkReply run(const QString &command, const QString &requestId)
    {
        // Checked here as well as in setScannerProgramPath() so an injected path that never
        // passed through the setter still gets an actionable reason rather than
        // QProcess's generic "failed to start".
        if (m_program.trimmed().isEmpty() || !QFileInfo::exists(m_program)) {
            BarcodeSdkReply out;
            out.status = BarcodeSdkStatus::ProgramUnavailable;
            return out;
        }
        QStringList arguments;
        // The endpoint is passed EXPLICITLY. The C example treats an empty
        // argument as L"" (not NULL), and only NULL is documented to select the
        // default endpoint — so an empty string is a needless bet. This literal
        // is the vendor's default endpoint name (BarcodeReaderTrigger.h:26) and
        // the exact spelling the operator's own command line uses.
        arguments << QStringLiteral("BarcodeReader.Trigger.v1") << command;
        if (!requestId.isEmpty())
            arguments << requestId;

        QProcess process;
#ifdef _WIN32
        // Without this every cycle flashes a console window over the HMI.
        process.setCreateProcessArgumentsModifier(
            [](QProcess::CreateProcessArguments *args) {
                args->flags |= CREATE_NO_WINDOW;
            });
#endif
        // The program and every argument are passed separately: no shell is
        // involved, so a path containing spaces still works and no value is
        // ever re-parsed as command-line syntax.
        process.start(m_program, arguments);
        if (!process.waitForStarted(5000)) {
            BarcodeSdkReply out;
            out.status = BarcodeSdkStatus::ProgramUnavailable;
            return out;
        }
        if (!process.waitForFinished(static_cast<int>(m_callTimeoutMs))) {
            // A wedged CLI must not hold the cycle open. The decode it may have
            // started is NOT cancelled: the cycle keeps querying the SAME
            // request id, which is exactly the documented recovery.
            process.kill();
            process.waitForFinished(1000);
            BarcodeSdkReply out;
            out.status = BarcodeSdkStatus::Timeout;
            return out;
        }
        BarcodeSdkReply out;
        if (process.exitStatus() != QProcess::NormalExit) {
            // Killed by a signal / terminated abnormally: no transport code to
            // read, and the call did not complete.
            out.status = BarcodeSdkStatus::Timeout;
            return out;
        }
        const int code = process.exitCode();
        if (code < 0 || code > kBrResponseTooLarge) {
            // Not a BR_* code at all (a crash, a wrapper's own exit code):
            // treat it as "could not talk to the scanner program".
            out.status = BarcodeSdkStatus::ProgramUnavailable;
            return out;
        }
        out.status = static_cast<BarcodeSdkStatus>(code);
        if (out.status == BarcodeSdkStatus::Ok) {
            // `puts(response)` ends the JSON with one newline and nothing else,
            // so stdout is the payload. Trim defensively anyway: a future CLI
            // that adds CRLF must not turn into "every reply failed to parse".
            out.json = process.readAllStandardOutput().trimmed();
        }
        return out;
    }

    QString m_program;
    qint64 m_callTimeoutMs = 2000;
};

std::unique_ptr<IBarcodeSdk> makeSystemBarcodeSdk(const QString &scannerProgramPath)
{
    return std::make_unique<BarcodeCliClient>(scannerProgramPath, kCallTimeoutMs);
}

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

void BarcodeReaderSdkSource::setScannerProgramPath(const QString &path)
{
    QMutexLocker lock(&m_mutex);
    const QString trimmed = path.trimmed();
    if (trimmed == m_scannerProgramPath)
        return;
    m_scannerProgramPath = trimmed;
    // Only a flag here: the module itself is loaded and freed on the worker
    // thread, at the start of the next cycle (applyPendingScannerProgramPath). Doing it
    // from this (caller) thread could free the library under an in-flight call.
    m_scannerProgramDirty = true;
}

QString BarcodeReaderSdkSource::scannerProgramPath() const
{
    QMutexLocker lock(&m_mutex);
    return m_scannerProgramPath;
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

void BarcodeReaderSdkSource::applyPendingScannerProgramPath()
{
    QString path;
    {
        QMutexLocker lock(&m_mutex);
        if (!m_scannerProgramDirty)
            return;
        m_scannerProgramDirty = false;
        path = m_scannerProgramPath;
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
    applyPendingScannerProgramPath();

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

    // The sequence is taken under the same lock as m_sequence, so two cycles can
    // never build the same timestamp-suffixed id even inside one millisecond.
    quint64 idSequence = 0;
    {
        QMutexLocker lock(&m_mutex);
        idSequence = ++m_idSequence;
    }
    m_requestId = makeRequestId(idSequence);
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

    QStringList arguments;
    if (!barcodeForwardArguments(result->rows, &arguments)) {
        result->forwardDetail =
            QStringLiteral("条码含制表符或换行，外发协议无法表示，本轮未外发");
        return;
    }
    if (arguments.isEmpty()) {
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
    // The program path and every argument are passed separately: no shell is
    // involved, so a path containing spaces still works and no barcode value is
    // ever re-parsed as command-line syntax.
    process.start(program, arguments);
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
        return QStringLiteral("扫码服务未启动，请先打开 BarcodeReader 并点击运行");
    case BarcodeSdkStatus::ProgramUnavailable:
        // The program itself could not be started — a deployment problem, not a
        // "start the scan program" one, so it gets its own actionable wording.
        return QStringLiteral("扫码程序无法启动，请检查扫码程序路径");
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
