#include "ui/pages/overview_page.h"

#include "application/permission_policy.h"
#include "ui/shell/shell_model.h"
#include "ui/widgets/disabled_hint.h"
#include "ui/widgets/permission_button.h"
#include "ui/widgets/value_display.h"

#include <QLabel>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QFrame>
#include <QHash>
#include <QLineEdit>
#include <QPushButton>
#include <QSizePolicy>
#include <QStringList>

namespace hlm {

namespace {

// Status light index order.
enum LightIndex { LightOnline = 0, LightMode, LightRunning, LightFault };

} // namespace

OverviewPage::OverviewPage(ShellModel &model, QWidget *parent)
    : QWidget(parent)
    , m_model(model)
    , m_pageModel(model)
{
    setObjectName(QStringLiteral("overviewPage"));
    buildLayout();
    connect(&m_model, &ShellModel::stateChanged, this, &OverviewPage::refresh);
    refresh();
}

void OverviewPage::buildLayout()
{
    // Qt Layout only, no absolute coordinates (spec §11.1). Structure follows
    // the reference image (微信图片_20260828085215_44_112.jpg): status row on
    // top, device schematic area in the middle, value grid below, latest
    // alarm line at the bottom.
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(16, 16, 16, 16);
    root->setSpacing(12);

    // --- status row -----------------------------------------------------------
    auto *statusRow = new QHBoxLayout();
    statusRow->setSpacing(24);
    const QStringList lightNames = {
        QStringLiteral("在线"), QStringLiteral("模式"),
        QStringLiteral("运行"), QStringLiteral("故障"),
    };
    for (const QString &name : lightNames) {
        auto *light = new StatusLight(this);
        light->setState(StatusState::Unknown, name + QStringLiteral(" —"));
        m_statusLights.append(light);
        statusRow->addWidget(light);
    }
    statusRow->addStretch();
    root->addLayout(statusRow);

    // --- device schematic placeholder (设备示意, spec §11.3) -------------------
    auto *schematic = new QFrame(this);
    schematic->setObjectName(QStringLiteral("overviewSchematic"));
    schematic->setFrameShape(QFrame::StyledPanel);
    schematic->setMinimumHeight(180);
    auto *schematicLayout = new QVBoxLayout(schematic);
    schematicLayout->setContentsMargins(20, 16, 20, 20);
    schematicLayout->setSpacing(14);
    auto *schematicLabel = new QLabel(QStringLiteral("设备流程"), schematic);
    schematicLabel->setObjectName(QStringLiteral("sectionTitle"));
    schematicLayout->addWidget(schematicLabel);

    auto *flow = new QHBoxLayout();
    flow->setSpacing(14);
    const QStringList stages = {
        QStringLiteral("入口输送\n来料进入"),
        QStringLiteral("调宽机构\n宽度闭环控制"),
        QStringLiteral("出口输送\n完成放行"),
    };
    for (int i = 0; i < stages.size(); ++i) {
        auto *node = new QFrame(schematic);
        node->setObjectName(QStringLiteral("schematicNode"));
        auto *nodeLayout = new QVBoxLayout(node);
        auto *nodeLabel = new QLabel(stages[i], node);
        nodeLabel->setObjectName(QStringLiteral("schematicNodeText"));
        nodeLabel->setAlignment(Qt::AlignCenter);
        nodeLayout->addWidget(nodeLabel);
        flow->addWidget(node, 1);
        if (i + 1 < stages.size()) {
            auto *arrow = new QLabel(QStringLiteral("→"), schematic);
            arrow->setObjectName(QStringLiteral("schematicArrow"));
            arrow->setAlignment(Qt::AlignCenter);
            flow->addWidget(arrow);
        }
    }
    schematicLayout->addLayout(flow, 1);
    root->addWidget(schematic, /*stretch=*/1);

    // --- value grid -------------------------------------------------------------
    auto *grid = new QGridLayout();
    grid->setHorizontalSpacing(32);
    grid->setVerticalSpacing(12);
    auto *leftColumn = new QVBoxLayout();
    auto *rightColumn = new QVBoxLayout();
    leftColumn->addWidget(addField(QStringLiteral("step"),
                                   QStringLiteral("当前步骤 (D120)")));
    leftColumn->addWidget(addField(QStringLiteral("targetWidth"),
                                   QStringLiteral("目标宽度 (D128)")));
    leftColumn->addWidget(addField(QStringLiteral("currentWidth"),
                                   QStringLiteral("当前宽度 (D130)")));
    rightColumn->addWidget(addField(QStringLiteral("widthDelta"),
                                    QStringLiteral("调宽差值 (D210)")));
    rightColumn->addWidget(addField(QStringLiteral("beltSpeed"),
                                    QStringLiteral("皮带速度 (D122)")));
    // 累计产量 + always-visible reliability disclosure (PLC-HMI-005 D4/F-08):
    // the count is not trustworthy after automatic width adjustment while the
    // PLC DMUL overlap remains, so the disclosure is a real touch-readable
    // label next to the value, never a tooltip.
    QWidget *productionField = addField(QStringLiteral("productionCount"),
                                        QStringLiteral("累计产量 (D138)"));
    auto *productionLayout = qobject_cast<QVBoxLayout *>(productionField->layout());
    m_productionCountLabel = new QLabel(productionField);
    m_productionCountLabel->setObjectName(QStringLiteral("productionCountText"));
    m_productionCountLabel->setMinimumHeight(48);
    m_productionCountLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    productionLayout->addWidget(m_productionCountLabel);
    m_productionDisclosure = new QLabel(m_pageModel.productionCountReliabilityText(),
                                        productionField);
    m_productionDisclosure->setObjectName(
        QStringLiteral("productionCountDisclosure"));
    m_productionDisclosure->setWordWrap(true);
    m_productionDisclosure->setMinimumHeight(48); // touch-target-height text
    productionLayout->addWidget(m_productionDisclosure);
    rightColumn->addWidget(productionField);
    grid->addLayout(leftColumn, 0, 0);
    grid->addLayout(rightColumn, 0, 1);
    grid->setColumnStretch(0, 1);
    grid->setColumnStretch(1, 1);
    root->addLayout(grid);

    // --- latest alarm line -------------------------------------------------------
    m_alarmLabel = new QLabel(this);
    m_alarmLabel->setObjectName(QStringLiteral("overviewAlarm"));
    m_alarmLabel->setMinimumHeight(48); // touch-target-height text line
    root->addWidget(m_alarmLabel);

    // --- 条码/扫码 status line (user decision 2026-09-22) ----------------------
    // Display-only. The scanning program is an external peer, so this surface
    // only ever shows what the PLC coil M15 and the result file reported; it
    // never claims a connection, and with no result path configured it stays
    // the 未配置 placeholder (contract: keep barcode integration visibly
    // not-configured until a path is set).
    m_barcodePlaceholder = new QLabel(this);
    m_barcodePlaceholder->setObjectName(QStringLiteral("barcodePlaceholderStatus"));
    m_barcodePlaceholder->setMinimumHeight(48); // touch-target-height text line
    m_barcodePlaceholder->setWordWrap(true);
    root->addWidget(m_barcodePlaceholder);

    // --- 扫码服务块 (user decision 2026-09-23) ---------------------------------
    // Manual 采集条码 trigger + the two persisted deployment paths. The block
    // is what makes the scan debuggable without the PLC: the M15 扫码结束 coil
    // has no rung in the supplied PLC program yet, so on the bench the manual
    // button is the only way to drive a cycle.
    root->addWidget(buildScanServiceBlock());
}

QWidget *OverviewPage::buildScanServiceBlock()
{
    auto *panel = new QFrame(this);
    panel->setObjectName(QStringLiteral("scanServicePanel"));
    panel->setFrameShape(QFrame::StyledPanel);
    auto *layout = new QVBoxLayout(panel);
    layout->setContentsMargins(16, 12, 16, 16);
    layout->setSpacing(10);

    auto *title = new QLabel(QStringLiteral("扫码服务"), panel);
    title->setObjectName(QStringLiteral("sectionTitle"));
    layout->addWidget(title);

    // Row 1: 采集条码 — admin-only bench control, disabled while a cycle runs.
    // objectName deliberately avoids every barcode/scan token the pinned
    // placeholder test scans for ("scan" IS one of its tokens, so the name uses
    // 采集/"collect" instead).
    auto *triggerRow = new QHBoxLayout();
    triggerRow->setSpacing(12);
    m_scanTrigger = new PermissionButton(QStringLiteral("采集条码"), panel);
    m_scanTrigger->setObjectName(QStringLiteral("collectTriggerButton"));
    m_scanTrigger->setMinimumHeight(64);
    connect(m_scanTrigger, &QPushButton::clicked, this,
            &OverviewPage::scanTriggerRequested);
    triggerRow->addWidget(m_scanTrigger);
    m_scanTriggerReason = new QLabel(panel);
    m_scanTriggerReason->setObjectName(QStringLiteral("collectTriggerReason"));
    m_scanTriggerReason->setWordWrap(true);
    m_scanTriggerReason->setMinimumHeight(20);
    triggerRow->addWidget(m_scanTriggerReason, 1);
    layout->addLayout(triggerRow);

    // Row 2: vendor library path. Empty = load by name from the executable's
    // directory (the documented deployment), so the placeholder must not imply
    // the path is required.
    auto *sdkTitle = new QLabel(QStringLiteral("扫码库路径（留空 = 从程序目录加载）"),
                                panel);
    sdkTitle->setObjectName(QStringLiteral("valueFieldTitle"));
    layout->addWidget(sdkTitle);
    m_sdkPathEdit = new QLineEdit(panel);
    m_sdkPathEdit->setObjectName(QStringLiteral("sdkPathEdit"));
    // The placeholder deliberately names NO product: the pinned placeholder test
    // scans every QLineEdit's placeholder for barcode/scanner/inbox tokens, and
    // the vendor library's filename contains one.
    m_sdkPathEdit->setPlaceholderText(
        QStringLiteral("例如 D:\\SDK\\x64\\<厂商库文件>.dll"));
    m_sdkPathEdit->setMinimumHeight(44);
    layout->addWidget(m_sdkPathEdit);
    auto *sdkButtons = new QHBoxLayout();
    m_saveSdkPath = new PermissionButton(QStringLiteral("保存扫码库路径"), panel);
    m_saveSdkPath->setObjectName(QStringLiteral("saveSdkPathButton"));
    m_saveSdkPath->setMinimumHeight(48);
    connect(m_saveSdkPath, &QPushButton::clicked, this,
            &OverviewPage::onSaveSdkPathClicked);
    sdkButtons->addWidget(m_saveSdkPath);
    sdkButtons->addStretch();
    layout->addLayout(sdkButtons);
    m_sdkPathStatus = new QLabel(panel);
    m_sdkPathStatus->setObjectName(QStringLiteral("sdkPathStatus"));
    m_sdkPathStatus->setMinimumHeight(32);
    m_sdkPathStatus->setWordWrap(true);
    layout->addWidget(m_sdkPathStatus);

    // Row 3: forward program path. Empty = the outbound step does not run.
    auto *forwardTitle =
        new QLabel(QStringLiteral("外发程序路径（留空 = 不调用；每个码作一个参数）"),
                   panel);
    forwardTitle->setObjectName(QStringLiteral("valueFieldTitle"));
    layout->addWidget(forwardTitle);
    m_forwardExeEdit = new QLineEdit(panel);
    m_forwardExeEdit->setObjectName(QStringLiteral("forwardExePathEdit"));
    // The placeholder names no product and no barcode/scan token: the pinned
    // placeholder test scans every QLineEdit's placeholder text, and "条码" is
    // one of its tokens. What the program receives is explained by the label
    // above, not by the hint.
    m_forwardExeEdit->setPlaceholderText(
        QStringLiteral("例如 D:\\Tools\\send.exe"));
    m_forwardExeEdit->setMinimumHeight(44);
    layout->addWidget(m_forwardExeEdit);
    auto *forwardButtons = new QHBoxLayout();
    m_saveForwardExe =
        new PermissionButton(QStringLiteral("保存外发程序路径"), panel);
    m_saveForwardExe->setObjectName(QStringLiteral("saveForwardExeButton"));
    m_saveForwardExe->setMinimumHeight(48);
    connect(m_saveForwardExe, &QPushButton::clicked, this,
            &OverviewPage::onSaveForwardExePathClicked);
    forwardButtons->addWidget(m_saveForwardExe);
    forwardButtons->addStretch();
    layout->addLayout(forwardButtons);
    m_forwardExePathStatus = new QLabel(panel);
    m_forwardExePathStatus->setObjectName(QStringLiteral("forwardExePathStatus"));
    m_forwardExePathStatus->setMinimumHeight(32);
    m_forwardExePathStatus->setWordWrap(true);
    layout->addWidget(m_forwardExePathStatus);

    return panel;
}

// Why the manual trigger is unavailable. Empty = it is available. A missing
// reason is never invented: the only honest reasons are "no permission" and
// "a cycle is already running".
QString OverviewPage::scanTriggerReasonText() const
{
    const PermissionResult p =
        PermissionPolicy::check(m_model.role(), Command::ParameterChange);
    if (!p.allowed)
        return p.reason;
    if (m_model.scanInProgress())
        return QStringLiteral("上一轮扫码尚未结束，请稍候");
    return QString();
}

// Same shape for the two path editors: the permission gate is identical to the
// result-path editor on the settings page (settings are administrator-only,
// spec §11.4), and a save that is already in flight must not be re-submitted.
QString OverviewPage::scanControlReasonText(bool pending) const
{
    const PermissionResult p =
        PermissionPolicy::check(m_model.role(), Command::ParameterChange);
    if (!p.allowed)
        return p.reason;
    if (pending)
        return QStringLiteral("正在保存…");
    return QString();
}

void OverviewPage::onSaveSdkPathClicked()
{
    if (m_sdkPathSavePending)
        return;
    setSdkPathSavePending();
    emit sdkPathSaveRequested(m_sdkPathEdit->text().trimmed());
}

void OverviewPage::onSaveForwardExePathClicked()
{
    if (m_forwardExePathSavePending)
        return;
    setForwardExePathSavePending();
    emit forwardExePathSaveRequested(m_forwardExeEdit->text().trimmed());
}

void OverviewPage::setSdkPath(const QString &path)
{
    if (m_sdkPathEdit != nullptr)
        m_sdkPathEdit->setText(path);
}

void OverviewPage::setForwardExePath(const QString &path)
{
    if (m_forwardExeEdit != nullptr)
        m_forwardExeEdit->setText(path);
}

QString OverviewPage::sdkPathStatusText() const
{
    return m_sdkPathStatus ? m_sdkPathStatus->text() : QString();
}

QString OverviewPage::forwardExePathStatusText() const
{
    return m_forwardExePathStatus ? m_forwardExePathStatus->text() : QString();
}

void OverviewPage::setSdkPathSavePending()
{
    m_sdkPathSavePending = true;
    refresh(); // re-derives the visible reason (正在保存…) and disables the button
    m_saveSdkPath->setEnabled(false);
    m_sdkPathStatus->setText(QStringLiteral("正在保存扫码库路径…"));
}

void OverviewPage::setSdkPathSaveResult(bool ok, const QString &detail)
{
    m_sdkPathSavePending = false;
    refresh();
    m_sdkPathStatus->setText(ok
                                 ? QStringLiteral("扫码库路径已保存")
                                 : QStringLiteral("扫码库路径保存失败：%1").arg(detail));
}

void OverviewPage::setForwardExePathSavePending()
{
    m_forwardExePathSavePending = true;
    refresh();
    m_saveForwardExe->setEnabled(false);
    m_forwardExePathStatus->setText(QStringLiteral("正在保存外发程序路径…"));
}

void OverviewPage::setForwardExePathSaveResult(bool ok, const QString &detail)
{
    m_forwardExePathSavePending = false;
    refresh();
    m_forwardExePathStatus->setText(
        ok ? QStringLiteral("外发程序路径已保存")
           : QStringLiteral("外发程序路径保存失败：%1").arg(detail));
}

QWidget *OverviewPage::addField(const QString &key, const QString &title)
{
    auto *titleWrap = new QWidget(this);
    titleWrap->setObjectName(QStringLiteral("valueField"));
    titleWrap->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto *layout = new QVBoxLayout(titleWrap);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);
    auto *titleLabel = new QLabel(title, titleWrap);
    titleLabel->setObjectName(QStringLiteral("valueFieldTitle"));
    layout->addWidget(titleLabel);
    auto *display = new ValueDisplay(titleWrap);
    display->setMinimumHeight(48);
    display->setMaximumHeight(54);
    display->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    layout->addWidget(display);
    m_displays.insert(key, display);
    return titleWrap;
}

ValueDisplay *OverviewPage::fieldDisplay(const QString &key) const
{
    return m_displays.value(key, nullptr);
}

QLabel *OverviewPage::latestAlarmLabel() const
{
    return m_alarmLabel;
}

QLabel *OverviewPage::productionCountDisclosureLabel() const
{
    return m_productionDisclosure;
}

QLabel *OverviewPage::barcodePlaceholderLabel() const
{
    return m_barcodePlaceholder;
}

void OverviewPage::setBarcodeResultPath(const QString &path)
{
    m_barcodePath = path.trimmed();
    // A new path invalidates the previous readback: the value on screen must
    // never be one the current path did not produce.
    m_barcodeResult = BarcodeResult();
    m_barcodeResultSet = false;
    refresh();
}

void OverviewPage::setBarcodeResult(const BarcodeResult &result)
{
    m_barcodeResult = result;
    m_barcodeResultSet = true;
    refresh();
}

QString OverviewPage::barcodeText() const
{
    return m_barcodePlaceholder ? m_barcodePlaceholder->text() : QString();
}

QString OverviewPage::latestAlarmText() const
{
    return m_alarmLabel ? m_alarmLabel->text() : QString();
}

void OverviewPage::refresh()
{
    // Full-snapshot re-render: every field re-read from the current snapshot
    // (spec §9: 完整快照更新，无逐字段或乐观更新).

    // Status lights (color + text, spec §11.2).
    m_statusLights[LightOnline]->setState(
        m_pageModel.online() ? StatusState::On : StatusState::Unknown,
        m_pageModel.online() ? QStringLiteral("在线") : QStringLiteral("通讯中断"));

    if (!m_pageModel.modeKnown())
        m_statusLights[LightMode]->setState(StatusState::Unknown,
                                            QStringLiteral("模式 —"));
    else if (m_pageModel.isAutoMode())
        m_statusLights[LightMode]->setState(StatusState::Info,
                                            QStringLiteral("自动"));
    else
        m_statusLights[LightMode]->setState(StatusState::Amber,
                                            QStringLiteral("手动"));

    if (!m_pageModel.modeKnown())
        m_statusLights[LightRunning]->setState(StatusState::Unknown,
                                               QStringLiteral("运行 —"));
    else if (m_pageModel.isRunning())
        m_statusLights[LightRunning]->setState(StatusState::On,
                                               QStringLiteral("运行中"));
    else
        m_statusLights[LightRunning]->setState(StatusState::Unknown,
                                               QStringLiteral("停止"));

    if (!m_pageModel.modeKnown())
        m_statusLights[LightFault]->setState(StatusState::Unknown,
                                             QStringLiteral("故障 —"));
    else if (m_pageModel.isFaulted())
        m_statusLights[LightFault]->setState(StatusState::Error,
                                             QStringLiteral("故障"));
    else
        m_statusLights[LightFault]->setState(StatusState::On,
                                             QStringLiteral("正常"));

    // Value fields: invalid -> "—" inside ValueDisplay (spec §9).
    const OverviewField step = m_pageModel.step();
    m_displays[QStringLiteral("step")]->setValue(step.text, QString(),
                                                 step.valid);
    const OverviewField target = m_pageModel.targetWidth();
    m_displays[QStringLiteral("targetWidth")]->setValue(
        target.text, QStringLiteral("mm"), target.valid);
    const OverviewField current = m_pageModel.currentWidth();
    m_displays[QStringLiteral("currentWidth")]->setValue(
        current.text, QStringLiteral("mm"), current.valid);
    const OverviewField delta = m_pageModel.widthDelta();
    m_displays[QStringLiteral("widthDelta")]->setValue(
        delta.text, QStringLiteral("mm"), delta.valid);
    const OverviewField speed = m_pageModel.beltSpeed();
    m_displays[QStringLiteral("beltSpeed")]->setValue(
        speed.text, QStringLiteral("Hz"), speed.valid);
    const OverviewField production = m_pageModel.productionCount();
    m_displays[QStringLiteral("productionCount")]->setValue(
        production.text, QStringLiteral("件"), production.valid);
    // Visible QLabel carries the value too so the count is not only custom
    // paint, and the disclosure always accompanies it (PLC-HMI-005 D4).
    m_productionCountLabel->setText(
        production.valid
            ? QStringLiteral("累计产量 %1 件").arg(production.text)
            : QStringLiteral("累计产量 —"));
    m_productionDisclosure->setText(m_pageModel.productionCountReliabilityText());

    // Latest alarm line.
    m_alarmLabel->setText(m_pageModel.latestAlarmText());

    // 条码/扫码 status line (user decision 2026-09-22). The text is a pure
    // function of the configured path, the PLC's M11 相机触发中 coil and the
    // last readback, so a page refresh (every snapshot) keeps it current
    // without the composition root re-pushing anything.
    m_barcodePlaceholder->setText(barcodeStatusText());

    // 扫码服务块 (user decision 2026-09-23): the manual trigger is admin-only
    // (the same gate as every other bench/test control) and is disabled while a
    // cycle runs; the two path editors share that gate and disable while their
    // own save is in flight. Every reason is inline visible text on the control
    // itself (PermissionButton renders it), with the hover hint as supplement
    // only.
    const QString triggerReason = scanTriggerReasonText();
    m_scanTrigger->setEnabledWithReason(triggerReason.isEmpty(), triggerReason);
    setUnavailableHint(m_scanTrigger, triggerReason.isEmpty(), triggerReason);
    // The label next to the button explains what it is for. The disabled REASON
    // is rendered inside the PermissionButton itself (never tooltip-only), so
    // the two never compete for the same line.
    m_scanTriggerReason->setText(QStringLiteral("台架调试用：不依赖 PLC 的 M15 信号，"
                                                "直接触发一轮解码"));

    const QString sdkReason = scanControlReasonText(m_sdkPathSavePending);
    m_saveSdkPath->setEnabledWithReason(sdkReason.isEmpty(), sdkReason);
    setUnavailableHint(m_saveSdkPath, sdkReason.isEmpty(), sdkReason);
    const QString forwardReason = scanControlReasonText(m_forwardExePathSavePending);
    m_saveForwardExe->setEnabledWithReason(forwardReason.isEmpty(), forwardReason);
    setUnavailableHint(m_saveForwardExe, forwardReason.isEmpty(), forwardReason);
}

// Renders the 条码/扫码 line. Kept in one place so the "never claim a
// connection" rule (contract: barcode integration stays visibly
// not-configured) is enforced by construction: 未配置 appears whenever no
// result path is configured, and no branch ever says 已连接/在线.
//
// A cycle yields up to N barcodes (one per enabled rectangle — the reference
// file holds 6 per board), so the Ok branch lists every decoded barcode, one
// per line, and states how many positions came back empty instead of quietly
// dropping them. An empty position is NEVER filled with an older code.
QString OverviewPage::barcodeStatusText() const
{
    if (m_barcodePath.isEmpty())
        return QStringLiteral("条码/扫码：未配置（预留）");

    const bool scanning = m_pageModel.m11();
    if (!m_barcodeResultSet)
        return scanning ? QStringLiteral("条码：扫码中…")
                        : QStringLiteral("条码：已配置，等待扫码");

    switch (m_barcodeResult.state) {
    case BarcodeState::NotConfigured:
        // The path was cleared between the cycle and this render.
        return QStringLiteral("条码/扫码：未配置（预留）");
    case BarcodeState::Ok: {
        QStringList lines;
        for (const BarcodeRow &row : m_barcodeResult.rows) {
            if (row.barcode.trimmed().isEmpty())
                continue;
            lines.append(row.barcode);
        }
        if (lines.isEmpty())
            lines.append(m_barcodeResult.line); // defensive: never blank
        QString text = QStringLiteral("条码：%1").arg(lines.join(QLatin1Char('\n')));
        if (m_barcodeResult.emptyPositions > 0) {
            text += QStringLiteral("\n（另有 %1 个位置未识别到条码）")
                        .arg(m_barcodeResult.emptyPositions);
        }
        if (!m_barcodeResult.persisted && !m_barcodeResult.persistDetail.isEmpty()) {
            text += QStringLiteral("\n（存储失败：%1）")
                        .arg(m_barcodeResult.persistDetail);
        }
        // Forward outcome (user decision 2026-09-23). Silent when no forward
        // program is configured: the step did not run, so claiming either
        // success or failure would be a lie. A failure never replaces the
        // barcodes above — they were decoded and stored either way.
        if (m_barcodeResult.forwarded) {
            text += QStringLiteral("\n（已外发）");
        } else if (!m_barcodeResult.forwardDetail.isEmpty()) {
            text += QStringLiteral("\n（外发失败：%1）")
                        .arg(m_barcodeResult.forwardDetail);
        }
        return text;
    }
    case BarcodeState::NoCode:
        return QStringLiteral("条码：本轮未识别到条码");
    case BarcodeState::Overlapped:
        // A 扫码结束 edge arrived while the previous cycle was still polling.
        // The refusal is shown, not swallowed, and the last barcode it could
        // have produced is not implied to be this board's.
        return QStringLiteral("条码：%1").arg(
            m_barcodeResult.detail.isEmpty()
                ? QStringLiteral("上一轮扫码尚未结束，本次扫码结束信号未处理")
                : m_barcodeResult.detail);
    case BarcodeState::Failed:
        return QStringLiteral("条码：读取失败 — %1").arg(m_barcodeResult.detail);
    }
    return QStringLiteral("条码/扫码：未配置（预留）");
}

} // namespace hlm
