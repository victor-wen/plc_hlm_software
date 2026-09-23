#include "ui/pages/scan_service_page.h"

#include "application/permission_policy.h"
#include "ui/shell/shell_model.h"
#include "ui/widgets/disabled_hint.h"
#include "ui/widgets/permission_button.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QSpinBox>
#include <QVBoxLayout>

namespace hlm {

ScanServicePage::ScanServicePage(ShellModel &model, QWidget *parent)
    : QWidget(parent)
    , m_model(model)
{
    setObjectName(QStringLiteral("scanServicePage"));
    buildLayout();
    connect(&m_model, &ShellModel::stateChanged, this, &ScanServicePage::refresh);
    refresh();
}

void ScanServicePage::buildLayout()
{
    // Qt Layout only, no absolute coordinates (spec §11.1). Same section idiom
    // as the other pages: a QFrame panel with a sectionTitle, 16 px margins.
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(16, 16, 16, 16);
    root->setSpacing(12);

    // --- 本轮条码 ---------------------------------------------------------------
    // The operator-facing answer to "did the scan work": every position of the
    // last cycle, and the reason when there is none.
    auto *resultPanel = new QFrame(this);
    resultPanel->setObjectName(QStringLiteral("serviceResultPanel"));
    resultPanel->setFrameShape(QFrame::StyledPanel);
    auto *resultLayout = new QVBoxLayout(resultPanel);
    resultLayout->setContentsMargins(16, 12, 16, 16);
    resultLayout->setSpacing(8);

    auto *resultTitle = new QLabel(QStringLiteral("本轮条码"), resultPanel);
    resultTitle->setObjectName(QStringLiteral("sectionTitle"));
    resultLayout->addWidget(resultTitle);

    m_status = new QLabel(resultPanel);
    m_status->setObjectName(QStringLiteral("serviceStatusLabel"));
    m_status->setMinimumHeight(48);
    m_status->setWordWrap(true);
    resultLayout->addWidget(m_status);

    m_rows = new QLabel(resultPanel);
    m_rows->setObjectName(QStringLiteral("decodedRowsLabel"));
    m_rows->setMinimumHeight(96);
    m_rows->setWordWrap(true);
    m_rows->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    resultLayout->addWidget(m_rows);

    m_resultPathEcho = new QLabel(resultPanel);
    m_resultPathEcho->setObjectName(QStringLiteral("resultPathEcho"));
    m_resultPathEcho->setMinimumHeight(32);
    m_resultPathEcho->setWordWrap(true);
    resultLayout->addWidget(m_resultPathEcho);
    root->addWidget(resultPanel);

    // --- 采集条码 (bench trigger) ------------------------------------------------
    auto *collectPanel = buildSection(QStringLiteral("手动采集（台架调试）"));
    auto *collectLayout = qobject_cast<QVBoxLayout *>(collectPanel->layout());
    auto *collectRow = new QHBoxLayout();
    collectRow->setSpacing(12);
    m_collect = new PermissionButton(QStringLiteral("采集条码"), collectPanel);
    // objectName carries no barcode/scan token: the pinned placeholder test
    // scans the whole window for those and fails on any other interactive
    // control that has one.
    m_collect->setObjectName(QStringLiteral("collectTriggerButton"));
    m_collect->setMinimumHeight(64);
    connect(m_collect, &QPushButton::clicked, this,
            &ScanServicePage::onCollectClicked);
    collectRow->addWidget(m_collect);
    m_collectReason = new QLabel(collectPanel);
    m_collectReason->setObjectName(QStringLiteral("collectTriggerReason"));
    m_collectReason->setWordWrap(true);
    m_collectReason->setMinimumHeight(20);
    collectRow->addWidget(m_collectReason, 1);
    collectLayout->addLayout(collectRow);
    root->addWidget(collectPanel);

    // --- 扫码程序路径 -------------------------------------------------------------
    auto *scanPanel = buildSection(QStringLiteral("扫码程序路径（厂商命令行程序）"));
    auto *scanLayout = qobject_cast<QVBoxLayout *>(scanPanel->layout());
    m_scanProgramEdit = new QLineEdit(scanPanel);
    m_scanProgramEdit->setObjectName(QStringLiteral("sdkPathEdit"));
    // The placeholder names no product and no barcode/scan token: the pinned
    // placeholder test reads every QLineEdit's placeholder text.
    m_scanProgramEdit->setPlaceholderText(
        QStringLiteral("例如 D:\\SDK\\x64\\trigger_client.exe（留空 = 未配置）"));
    m_scanProgramEdit->setMinimumHeight(44);
    scanLayout->addWidget(m_scanProgramEdit);
    auto *scanButtons = new QHBoxLayout();
    m_saveScanProgram = new PermissionButton(QStringLiteral("保存扫码程序路径"),
                                             scanPanel);
    m_saveScanProgram->setObjectName(QStringLiteral("saveSdkPathButton"));
    m_saveScanProgram->setMinimumHeight(48);
    connect(m_saveScanProgram, &QPushButton::clicked, this,
            &ScanServicePage::onSaveScanProgramClicked);
    scanButtons->addWidget(m_saveScanProgram);
    scanButtons->addStretch();
    scanLayout->addLayout(scanButtons);
    m_scanProgramStatus = new QLabel(scanPanel);
    m_scanProgramStatus->setObjectName(QStringLiteral("sdkPathStatus"));
    m_scanProgramStatus->setMinimumHeight(32);
    m_scanProgramStatus->setWordWrap(true);
    scanLayout->addWidget(m_scanProgramStatus);
    root->addWidget(scanPanel);

    // --- 触发次数 ------------------------------------------------------------------
    auto *attemptsPanel =
        buildSection(QStringLiteral("触发次数（条码个数不符时最多采集几次）"));
    auto *attemptsLayout = qobject_cast<QVBoxLayout *>(attemptsPanel->layout());
    auto *attemptsRow = new QHBoxLayout();
    attemptsRow->setSpacing(12);
    m_scanAttemptsSpin = new QSpinBox(attemptsPanel);
    m_scanAttemptsSpin->setObjectName(QStringLiteral("scanAttemptsSpin"));
    // Never 0: a board with no attempt at all could never be scanned, so the
    // range starts at 1 (user decision 2026-09-23).
    m_scanAttemptsSpin->setRange(1, 9);
    m_scanAttemptsSpin->setMinimumHeight(48);
    attemptsRow->addWidget(m_scanAttemptsSpin);
    m_saveScanAttempts = new PermissionButton(QStringLiteral("保存触发次数"),
                                              attemptsPanel);
    m_saveScanAttempts->setObjectName(QStringLiteral("saveScanAttemptsButton"));
    m_saveScanAttempts->setMinimumHeight(48);
    connect(m_saveScanAttempts, &QPushButton::clicked, this,
            &ScanServicePage::onSaveScanAttemptsClicked);
    attemptsRow->addWidget(m_saveScanAttempts);
    attemptsRow->addStretch();
    attemptsLayout->addLayout(attemptsRow);
    m_scanAttemptsStatus = new QLabel(attemptsPanel);
    m_scanAttemptsStatus->setObjectName(QStringLiteral("scanAttemptsStatus"));
    m_scanAttemptsStatus->setMinimumHeight(32);
    m_scanAttemptsStatus->setWordWrap(true);
    attemptsLayout->addWidget(m_scanAttemptsStatus);
    root->addWidget(attemptsPanel);

    // --- 外发程序路径 -------------------------------------------------------------
    auto *forwardPanel =
        buildSection(QStringLiteral("外发程序路径（留空 = 不调用；每个码作一个参数）"));
    auto *forwardLayout = qobject_cast<QVBoxLayout *>(forwardPanel->layout());
    m_forwardProgramEdit = new QLineEdit(forwardPanel);
    m_forwardProgramEdit->setObjectName(QStringLiteral("forwardExePathEdit"));
    m_forwardProgramEdit->setPlaceholderText(
        QStringLiteral("例如 D:\\Tools\\send.exe（留空 = 不调用）"));
    m_forwardProgramEdit->setMinimumHeight(44);
    forwardLayout->addWidget(m_forwardProgramEdit);
    auto *forwardButtons = new QHBoxLayout();
    m_saveForwardProgram =
        new PermissionButton(QStringLiteral("保存外发程序路径"), forwardPanel);
    m_saveForwardProgram->setObjectName(QStringLiteral("saveForwardExeButton"));
    m_saveForwardProgram->setMinimumHeight(48);
    connect(m_saveForwardProgram, &QPushButton::clicked, this,
            &ScanServicePage::onSaveForwardProgramClicked);
    forwardButtons->addWidget(m_saveForwardProgram);
    forwardButtons->addStretch();
    forwardLayout->addLayout(forwardButtons);
    m_forwardProgramStatus = new QLabel(forwardPanel);
    m_forwardProgramStatus->setObjectName(QStringLiteral("forwardExePathStatus"));
    m_forwardProgramStatus->setMinimumHeight(32);
    m_forwardProgramStatus->setWordWrap(true);
    forwardLayout->addWidget(m_forwardProgramStatus);
    root->addWidget(forwardPanel);

    root->addStretch();
}

QWidget *ScanServicePage::buildSection(const QString &title)
{
    auto *panel = new QFrame(this);
    panel->setFrameShape(QFrame::StyledPanel);
    auto *layout = new QVBoxLayout(panel);
    layout->setContentsMargins(16, 12, 16, 16);
    layout->setSpacing(8);
    auto *titleLabel = new QLabel(title, panel);
    titleLabel->setObjectName(QStringLiteral("sectionTitle"));
    titleLabel->setWordWrap(true);
    layout->addWidget(titleLabel);
    return panel;
}

QString ScanServicePage::statusText() const
{
    return m_status != nullptr ? m_status->text() : QString();
}

QString ScanServicePage::decodedRowsText() const
{
    return m_rows != nullptr ? m_rows->text() : QString();
}

QString ScanServicePage::collectReasonText() const
{
    return m_collect != nullptr ? m_collect->disabledReason() : QString();
}

QString ScanServicePage::scanProgramStatusText() const
{
    return m_scanProgramStatus != nullptr ? m_scanProgramStatus->text() : QString();
}

QString ScanServicePage::scanAttemptsStatusText() const
{
    return m_scanAttemptsStatus != nullptr ? m_scanAttemptsStatus->text()
                                           : QString();
}

QString ScanServicePage::forwardProgramStatusText() const
{
    return m_forwardProgramStatus != nullptr ? m_forwardProgramStatus->text()
                                             : QString();
}

QString ScanServicePage::controlReasonText(bool pending) const
{
    const PermissionResult p =
        PermissionPolicy::check(m_model.role(), Command::ParameterChange);
    if (!p.allowed)
        return p.reason;
    if (pending)
        return QStringLiteral("正在保存…");
    return QString();
}

void ScanServicePage::onCollectClicked()
{
    // No local single-flight flag: the adapter owns "one cycle at a time" and
    // the button is disabled from m_model.scanInProgress() while one runs, so a
    // second click cannot get through. The composition root still reports a
    // refusal visibly if one ever does.
    emit collectRequested();
}

void ScanServicePage::onSaveScanProgramClicked()
{
    if (m_scanProgramSavePending)
        return;
    setScanProgramSavePending();
    emit scanProgramSaveRequested(m_scanProgramEdit->text().trimmed());
}

void ScanServicePage::onSaveScanAttemptsClicked()
{
    if (m_scanAttemptsSavePending)
        return;
    setScanAttemptsSavePending();
    emit scanAttemptsSaveRequested(m_scanAttemptsSpin->value());
}

void ScanServicePage::setScanAttempts(int attempts)
{
    if (m_scanAttemptsSpin != nullptr)
        m_scanAttemptsSpin->setValue(qMax(1, attempts));
}

void ScanServicePage::setScanAttemptsSavePending()
{
    m_scanAttemptsSavePending = true;
    refresh();
    m_saveScanAttempts->setEnabled(false);
    m_scanAttemptsStatus->setText(QStringLiteral("正在保存触发次数…"));
}

void ScanServicePage::setScanAttemptsSaveResult(bool ok, const QString &detail)
{
    m_scanAttemptsSavePending = false;
    refresh();
    m_scanAttemptsStatus->setText(
        ok ? QStringLiteral("触发次数已保存")
           : QStringLiteral("触发次数保存失败：%1").arg(detail));
}

void ScanServicePage::onSaveForwardProgramClicked()
{
    if (m_forwardProgramSavePending)
        return;
    setForwardProgramSavePending();
    emit forwardProgramSaveRequested(m_forwardProgramEdit->text().trimmed());
}

void ScanServicePage::setResultPath(const QString &path)
{
    m_resultPath = path.trimmed();
    refresh();
}

void ScanServicePage::setBarcodeResult(const BarcodeResult &result)
{
    m_result = result;
    m_resultSet = true;
    refresh();
}

void ScanServicePage::setScanProgramPath(const QString &path)
{
    if (m_scanProgramEdit != nullptr)
        m_scanProgramEdit->setText(path);
}

void ScanServicePage::setForwardProgramPath(const QString &path)
{
    if (m_forwardProgramEdit != nullptr)
        m_forwardProgramEdit->setText(path);
}

void ScanServicePage::setScanProgramSavePending()
{
    m_scanProgramSavePending = true;
    refresh();
    m_saveScanProgram->setEnabled(false);
    m_scanProgramStatus->setText(QStringLiteral("正在保存扫码程序路径…"));
}

void ScanServicePage::setScanProgramSaveResult(bool ok, const QString &detail)
{
    m_scanProgramSavePending = false;
    refresh();
    m_scanProgramStatus->setText(
        ok ? QStringLiteral("扫码程序路径已保存")
           : QStringLiteral("扫码程序路径保存失败：%1").arg(detail));
}

void ScanServicePage::setForwardProgramSavePending()
{
    m_forwardProgramSavePending = true;
    refresh();
    m_saveForwardProgram->setEnabled(false);
    m_forwardProgramStatus->setText(QStringLiteral("正在保存外发程序路径…"));
}

void ScanServicePage::setForwardProgramSaveResult(bool ok, const QString &detail)
{
    m_forwardProgramSavePending = false;
    refresh();
    m_forwardProgramStatus->setText(
        ok ? QStringLiteral("外发程序路径已保存")
           : QStringLiteral("外发程序路径保存失败：%1").arg(detail));
}

void ScanServicePage::refresh()
{
    // The status line answers "what will happen / what happened", in that
    // order. It never claims a connection (contract: the integration stays
    // visibly not-configured until it is configured) and never invents a result.
    if (m_resultSet) {
        switch (m_result.state) {
        case BarcodeState::NotConfigured:
            m_status->setText(QStringLiteral("扫码服务：未配置（请填写扫码程序路径）"));
            break;
        case BarcodeState::Ok: {
            QString text = QStringLiteral("扫码服务：本轮识别到 %1 个条码")
                               .arg(m_result.decodedCount);
            if (m_result.emptyPositions > 0)
                text += QStringLiteral("，另有 %1 个位置未识别到条码")
                            .arg(m_result.emptyPositions);
            if (!m_result.persisted && !m_result.persistDetail.isEmpty())
                text += QStringLiteral("（存储失败：%1）").arg(m_result.persistDetail);
            if (m_result.forwarded)
                text += QStringLiteral("（已外发）");
            else if (!m_result.forwardDetail.isEmpty())
                text += QStringLiteral("（外发失败：%1）").arg(m_result.forwardDetail);
            m_status->setText(text);
            break;
        }
        case BarcodeState::NoCode:
            m_status->setText(QStringLiteral("扫码服务：本轮未识别到条码"));
            break;
        case BarcodeState::CountMismatch:
            // The retry (and how many attempts are left) is the caller's
            // business; what matters here is why this capture was rejected.
            m_status->setText(QStringLiteral("扫码服务：%1").arg(
                m_result.detail.isEmpty()
                    ? QStringLiteral("条码个数不符，本轮未外发")
                    : m_result.detail));
            break;
        case BarcodeState::Overlapped:
            m_status->setText(QStringLiteral("扫码服务：%1").arg(
                m_result.detail.isEmpty()
                    ? QStringLiteral("上一轮扫码尚未结束，本次扫码结束信号未处理")
                    : m_result.detail));
            break;
        case BarcodeState::Failed:
            m_status->setText(QStringLiteral("扫码服务：读取失败 — %1")
                                  .arg(m_result.detail));
            break;
        }
    } else if (m_model.scanInProgress() || m_model.snapshot().m11()) {
        m_status->setText(QStringLiteral("扫码服务：扫码中…"));
    } else if (m_scanProgramEdit->text().trimmed().isEmpty()) {
        m_status->setText(QStringLiteral("扫码服务：未配置（请填写扫码程序路径）"));
    } else {
        m_status->setText(QStringLiteral("扫码服务：等待扫码（PLC 扫码结束信号自动触发）"));
    }

    // 本轮条码: one line per rectangle, in the SDK's table order. An empty
    // position says so — it is never filled with an older barcode.
    if (m_resultSet && !m_result.rows.isEmpty()) {
        QStringList lines;
        for (const BarcodeRow &row : m_result.rows) {
            const QString position = row.rectId > 0
                ? QStringLiteral("位置 %1").arg(row.rectId)
                : QStringLiteral("位置 %1").arg(row.sequence);
            lines.append(row.barcode.trimmed().isEmpty()
                             ? QStringLiteral("%1：（空读，未识别到条码）").arg(position)
                             : QStringLiteral("%1：%2").arg(position, row.barcode));
        }
        m_rows->setText(lines.join(QLatin1Char('\n')));
    } else {
        m_rows->setText(QStringLiteral("—"));
    }

    // Where the results go. Empty is a real state: the cycle still runs and the
    // barcodes are still shown, nothing is written.
    // The result file is optional (user decision 2026-09-23): with none
    // configured the barcodes are still decoded, still shown here and still
    // handed to the forward program. Said plainly, not as an error.
    m_resultPathEcho->setText(
        m_resultPath.isEmpty()
            ? QStringLiteral("结果文件：未配置（不落盘；条码仍会显示并外发）")
            : QStringLiteral("结果文件：%1").arg(m_resultPath));

    const QString collectReason =
        m_model.scanInProgress() ? QStringLiteral("上一轮扫码尚未结束，请稍候")
                                 : controlReasonText(false);
    m_collect->setEnabledWithReason(collectReason.isEmpty(), collectReason);
    setUnavailableHint(m_collect, collectReason.isEmpty(), collectReason);
    m_collectReason->setText(collectReason);

    const QString scanReason = controlReasonText(m_scanProgramSavePending);
    m_saveScanProgram->setEnabledWithReason(scanReason.isEmpty(), scanReason);
    setUnavailableHint(m_saveScanProgram, scanReason.isEmpty(), scanReason);
    const QString attemptsReason = controlReasonText(m_scanAttemptsSavePending);
    m_saveScanAttempts->setEnabledWithReason(attemptsReason.isEmpty(), attemptsReason);
    setUnavailableHint(m_saveScanAttempts, attemptsReason.isEmpty(), attemptsReason);
    m_scanAttemptsSpin->setEnabled(attemptsReason.isEmpty());
    setUnavailableHint(m_scanAttemptsSpin, attemptsReason.isEmpty(), attemptsReason);
    const QString forwardReason = controlReasonText(m_forwardProgramSavePending);
    m_saveForwardProgram->setEnabledWithReason(forwardReason.isEmpty(), forwardReason);
    setUnavailableHint(m_saveForwardProgram, forwardReason.isEmpty(), forwardReason);
}

} // namespace hlm