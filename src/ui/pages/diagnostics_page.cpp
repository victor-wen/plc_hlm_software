#include "ui/pages/diagnostics_page.h"

#include "ui/shell/shell_model.h"
#include "ui/widgets/value_display.h"
#include "ui/widgets/status_light.h"

#include <QLabel>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QTableWidget>
#include <QHeaderView>
#include <QScrollArea>
#include <QFrame>

namespace hlm {

namespace {

// Raw word table rows: D100, D102, D103, D104, D105 (spec §8.2).
const char *rawWordNames[] = {"D100 状态字1", "D102 状态字2", "D103 状态字3",
                              "D104 状态字4", "D105 状态字5"};

QTableWidget *makeTable(QWidget *parent)
{
    auto *table = new QTableWidget(parent);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionMode(QAbstractItemView::NoSelection);
    table->setFocusPolicy(Qt::NoFocus);
    table->verticalHeader()->setDefaultSectionSize(48); // spec §11.1
    table->verticalHeader()->setMinimumSectionSize(48);
    table->verticalHeader()->setVisible(false);
    table->horizontalHeader()->setStretchLastSection(true);
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    return table;
}

} // namespace

DiagnosticsPage::DiagnosticsPage(ShellModel &model, QWidget *parent)
    : QWidget(parent)
    , m_model(model)
    , m_pageModel(model)
{
    setObjectName(QStringLiteral("diagnosticsPage"));
    buildLayout();
    connect(&m_model, &ShellModel::stateChanged, this, &DiagnosticsPage::refresh);
    refresh();
}

void DiagnosticsPage::buildLayout()
{
    // Qt Layout only, no absolute coordinates (spec §11.1). The page is
    // scrollable so all sections stay reachable on smaller windows.
    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto *content = new QWidget(scroll);
    auto *root = new QVBoxLayout(content);
    root->setContentsMargins(16, 16, 16, 16);
    root->setSpacing(16);
    scroll->setWidget(content);

    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->addWidget(scroll);

    buildRawWordTable(root);
    buildBitTables(root);
    buildRegisterGrid(root);
    buildCommSection(root);
    buildVisionSection(root);
    root->addStretch();
}

void DiagnosticsPage::buildRawWordTable(QVBoxLayout *root)
{
    auto *title = new QLabel(QStringLiteral("原始状态字 (D100-D105)"), this);
    title->setMinimumHeight(48);
    root->addWidget(title);

    m_rawWordTable = makeTable(this);
    m_rawWordTable->setColumnCount(2);
    m_rawWordTable->setHorizontalHeaderLabels(
        {QStringLiteral("寄存器"), QStringLiteral("原始值 (hex)")});
    m_rawWordTable->setRowCount(5);
    for (int i = 0; i < 5; ++i) {
        m_rawWordTable->setItem(i, 0,
                                new QTableWidgetItem(QString::fromUtf8(rawWordNames[i])));
        m_rawWordTable->setItem(i, 1, new QTableWidgetItem(QStringLiteral("—")));
    }
    root->addWidget(m_rawWordTable);
}

void DiagnosticsPage::buildBitTables(QVBoxLayout *root)
{
    auto *title = new QLabel(QStringLiteral("已定义位状态"), this);
    title->setMinimumHeight(48);
    root->addWidget(title);

    auto *grid = new QGridLayout();
    grid->setHorizontalSpacing(16);
    grid->setVerticalSpacing(12);

    auto *d100Title = new QLabel(QStringLiteral("D100 位 (M0-M14)"), this);
    auto *d103Title = new QLabel(QStringLiteral("D103 位 (M30-M45)"), this);
    auto *homeTitle = new QLabel(QStringLiteral("M50-M53 / M100-M112"), this);
    d100Title->setMinimumHeight(48);
    d103Title->setMinimumHeight(48);
    homeTitle->setMinimumHeight(48);

    m_d100BitTable = makeTable(this);
    m_d100BitTable->setColumnCount(3);
    m_d100BitTable->setHorizontalHeaderLabels(
        {QStringLiteral("位"), QStringLiteral("名称"), QStringLiteral("状态")});
    m_d100BitTable->setRowCount(15);

    m_d103BitTable = makeTable(this);
    m_d103BitTable->setColumnCount(3);
    m_d103BitTable->setHorizontalHeaderLabels(
        {QStringLiteral("位"), QStringLiteral("名称"), QStringLiteral("状态")});
    m_d103BitTable->setRowCount(12);

    m_homeCommandTable = makeTable(this);
    m_homeCommandTable->setColumnCount(3);
    m_homeCommandTable->setHorizontalHeaderLabels(
        {QStringLiteral("位"), QStringLiteral("名称"), QStringLiteral("状态")});
    m_homeCommandTable->setRowCount(17);

    grid->addWidget(d100Title, 0, 0);
    grid->addWidget(m_d100BitTable, 1, 0);
    grid->addWidget(d103Title, 0, 1);
    grid->addWidget(m_d103BitTable, 1, 1);
    grid->addWidget(homeTitle, 2, 0);
    grid->addWidget(m_homeCommandTable, 3, 0, 1, 2);
    grid->setColumnStretch(0, 1);
    grid->setColumnStretch(1, 1);
    root->addLayout(grid);
}

void DiagnosticsPage::buildRegisterGrid(QVBoxLayout *root)
{
    auto *title = new QLabel(QStringLiteral("关键寄存器"), this);
    title->setMinimumHeight(48);
    root->addWidget(title);

    auto *grid = new QGridLayout();
    grid->setHorizontalSpacing(32);
    grid->setVerticalSpacing(12);
    auto *left = new QVBoxLayout();
    auto *right = new QVBoxLayout();

    left->addWidget(addRegister(QStringLiteral("faultCode"),
                                QStringLiteral("故障代码 (D110)"), QString(), nullptr));
    left->addWidget(addRegister(QStringLiteral("currentStep"),
                                QStringLiteral("当前步骤 (D120)"), QString(), nullptr));
    left->addWidget(addRegister(QStringLiteral("beltSpeed"),
                                QStringLiteral("皮带速度 (D122)"), QStringLiteral("Hz"), nullptr));
    left->addWidget(addRegister(QStringLiteral("widthFrequency"),
                                QStringLiteral("调宽频率 (D126/127)"), QStringLiteral("Hz"), nullptr));
    left->addWidget(addRegister(QStringLiteral("targetWidth"),
                                QStringLiteral("目标宽度 (D128)"), QStringLiteral("mm"), nullptr));
    left->addWidget(addRegister(QStringLiteral("currentWidth"),
                                QStringLiteral("当前宽度 (D130)"), QStringLiteral("mm"), nullptr));

    right->addWidget(addRegister(QStringLiteral("pulseCount"),
                                 QStringLiteral("调宽脉冲 (D136/137)"), QStringLiteral("脉冲"), nullptr));
    right->addWidget(addRegister(QStringLiteral("productionCount"),
                                 QStringLiteral("累计产量 (D138/139)"), QStringLiteral("件"), nullptr));
    right->addWidget(addRegister(QStringLiteral("pulsePerMm"),
                                 QStringLiteral("脉冲当量 (D204)"), QStringLiteral("脉冲/mm"), nullptr));
    right->addWidget(addRegister(QStringLiteral("widthDelta"),
                                 QStringLiteral("调宽差值 (D210)"), QStringLiteral("mm"), nullptr));
    right->addWidget(addRegister(QStringLiteral("widthSpeed"),
                                 QStringLiteral("调宽速度 (D220)"), QStringLiteral("mm/s"), nullptr));

    grid->addLayout(left, 0, 0);
    grid->addLayout(right, 0, 1);
    grid->setColumnStretch(0, 1);
    grid->setColumnStretch(1, 1);
    root->addLayout(grid);
}

void DiagnosticsPage::buildCommSection(QVBoxLayout *root)
{
    auto *title = new QLabel(QStringLiteral("通讯统计"), this);
    title->setMinimumHeight(48);
    root->addWidget(title);

    auto *row = new QHBoxLayout();
    row->setSpacing(24);

    m_heartbeatLight = new StatusLight(this);
    m_heartbeatLight->setState(StatusState::Unknown, QStringLiteral("心跳 —"));
    row->addWidget(m_heartbeatLight);

    auto *grid = new QGridLayout();
    grid->setHorizontalSpacing(32);
    grid->setVerticalSpacing(12);
    auto *left = new QVBoxLayout();
    auto *right = new QVBoxLayout();
    left->addWidget(addComm(QStringLiteral("latency"),
                            QStringLiteral("最近快照延迟"), QStringLiteral("ms"), nullptr));
    left->addWidget(addComm(QStringLiteral("sequence"),
                            QStringLiteral("快照序号"), QString(), nullptr));
    right->addWidget(addComm(QStringLiteral("reconnect"),
                             QStringLiteral("重连次数"), QString(), nullptr));
    right->addWidget(addComm(QStringLiteral("failedPolls"),
                             QStringLiteral("失败轮询"), QString(), nullptr));
    grid->addLayout(left, 0, 0);
    grid->addLayout(right, 0, 1);
    grid->setColumnStretch(0, 1);
    grid->setColumnStretch(1, 1);

    row->addLayout(grid, /*stretch=*/1);
    root->addLayout(row);
}

void DiagnosticsPage::buildVisionSection(QVBoxLayout *root)
{
    auto *title = new QLabel(QStringLiteral("OpenCV 自检"), this);
    title->setMinimumHeight(48);
    root->addWidget(title);

    auto *row = new QHBoxLayout();
    row->setSpacing(24);
    m_visionVersion = new QLabel(QStringLiteral("—"), this);
    m_visionVersion->setMinimumHeight(48);
    m_visionStatus = new QLabel(QStringLiteral("—"), this);
    m_visionStatus->setMinimumHeight(48);
    m_visionFailure = new QLabel(QStringLiteral("—"), this);
    m_visionFailure->setMinimumHeight(48);
    m_visionFailure->setWordWrap(true);

    row->addWidget(new QLabel(QStringLiteral("版本:"), this));
    row->addWidget(m_visionVersion);
    row->addWidget(new QLabel(QStringLiteral("状态:"), this));
    row->addWidget(m_visionStatus);
    row->addWidget(new QLabel(QStringLiteral("失败原因:"), this));
    row->addWidget(m_visionFailure, /*stretch=*/1);
    root->addLayout(row);
}

void DiagnosticsPage::fillBitTable(QTableWidget *table, const QVector<BitRow> &rows)
{
    table->setRowCount(rows.size());
    for (int i = 0; i < rows.size(); ++i) {
        const BitRow &r = rows.at(i);
        table->setItem(i, 0,
                       new QTableWidgetItem(QStringLiteral("M%1").arg(r.mNumber)));
        table->setItem(i, 1, new QTableWidgetItem(r.name));
        table->setItem(i, 2,
                       new QTableWidgetItem(r.known ? (r.state ? QStringLiteral("1")
                                                               : QStringLiteral("0"))
                                                    : QStringLiteral("—")));
    }
}

ValueDisplay *DiagnosticsPage::addRegister(const QString &key,
                                           const QString &title,
                                           const QString &unit,
                                           QVBoxLayout *column)
{
    Q_UNUSED(column);
    auto *wrap = new QWidget(this);
    auto *layout = new QVBoxLayout(wrap);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);
    auto *titleLabel = new QLabel(title, wrap);
    layout->addWidget(titleLabel);
    auto *display = new ValueDisplay(wrap);
    display->setMinimumHeight(48);
    layout->addWidget(display);
    m_registers.insert(key, display);
    return display;
}

ValueDisplay *DiagnosticsPage::addComm(const QString &key, const QString &title,
                                       const QString &unit, QVBoxLayout *column)
{
    Q_UNUSED(column);
    auto *wrap = new QWidget(this);
    auto *layout = new QVBoxLayout(wrap);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);
    auto *titleLabel = new QLabel(title, wrap);
    layout->addWidget(titleLabel);
    auto *display = new ValueDisplay(wrap);
    display->setMinimumHeight(48);
    layout->addWidget(display);
    m_comm.insert(key, display);
    return display;
}

ValueDisplay *DiagnosticsPage::registerDisplay(const QString &key) const
{
    return m_registers.value(key, nullptr);
}

ValueDisplay *DiagnosticsPage::commDisplay(const QString &key) const
{
    return m_comm.value(key, nullptr);
}

void DiagnosticsPage::setVisionStatus(const QString &version, bool healthy,
                                      const QString &failureReason)
{
    m_pageModel.setVisionStatus(version, healthy, failureReason);
    refresh();
}

void DiagnosticsPage::setCommStats(const CommStats &stats)
{
    m_pageModel.setCommStats(stats);
    refresh();
}

void DiagnosticsPage::refresh()
{
    // Full-snapshot re-render: every field re-read from the current snapshot
    // (spec §9: 完整快照更新，无逐字段或乐观更新).

    // Raw words (D100-D105). D102/D104/D105 are raw hex only (acceptance).
    const bool rawValid = m_pageModel.rawWordsValid();
    for (int i = 0; i < 5; ++i)
        m_rawWordTable->item(i, 1)->setText(m_pageModel.rawWordHex(i));
    Q_UNUSED(rawValid);

    // Defined bits.
    fillBitTable(m_d100BitTable, m_pageModel.d100Bits());
    fillBitTable(m_d103BitTable, m_pageModel.d103Bits());
    fillBitTable(m_homeCommandTable, m_pageModel.homeCommandBits());

    // Key registers.
    const DiagnosticsField fault = m_pageModel.faultCode();
    m_registers[QStringLiteral("faultCode")]->setValue(fault.text, QString(), fault.valid);
    const DiagnosticsField step = m_pageModel.currentStep();
    m_registers[QStringLiteral("currentStep")]->setValue(step.text, QString(), step.valid);
    const DiagnosticsField belt = m_pageModel.beltSpeed();
    m_registers[QStringLiteral("beltSpeed")]->setValue(belt.text, QStringLiteral("Hz"), belt.valid);
    const DiagnosticsField freq = m_pageModel.widthFrequency();
    m_registers[QStringLiteral("widthFrequency")]->setValue(freq.text, QStringLiteral("Hz"), freq.valid);
    const DiagnosticsField target = m_pageModel.targetWidth();
    m_registers[QStringLiteral("targetWidth")]->setValue(target.text, QStringLiteral("mm"), target.valid);
    const DiagnosticsField current = m_pageModel.currentWidth();
    m_registers[QStringLiteral("currentWidth")]->setValue(current.text, QStringLiteral("mm"), current.valid);
    const DiagnosticsField pulse = m_pageModel.pulseCount();
    m_registers[QStringLiteral("pulseCount")]->setValue(pulse.text, QStringLiteral("脉冲"), pulse.valid);
    const DiagnosticsField production = m_pageModel.productionCount();
    m_registers[QStringLiteral("productionCount")]->setValue(production.text, QStringLiteral("件"), production.valid);
    const DiagnosticsField ppm = m_pageModel.pulsePerMm();
    m_registers[QStringLiteral("pulsePerMm")]->setValue(ppm.text, QStringLiteral("脉冲/mm"), ppm.valid);
    const DiagnosticsField delta = m_pageModel.widthDelta();
    m_registers[QStringLiteral("widthDelta")]->setValue(delta.text, QStringLiteral("mm"), delta.valid);
    const DiagnosticsField speed = m_pageModel.widthSpeed();
    m_registers[QStringLiteral("widthSpeed")]->setValue(speed.text, QStringLiteral("mm/s"), speed.valid);

    // D140 heartbeat activity (spec §13).
    if (!m_pageModel.heartbeatKnown()) {
        m_heartbeatLight->setState(StatusState::Unknown, QStringLiteral("心跳 —"));
    } else if (m_pageModel.heartbeatActive()) {
        m_heartbeatLight->setState(StatusState::On, QStringLiteral("心跳 活性"));
    } else {
        m_heartbeatLight->setState(StatusState::Error, QStringLiteral("心跳 失效"));
    }

    // Comm statistics: latency/sequence from the snapshot, reconnect/failed
    // polls from the externally-fed CommStats (Task 20).
    const bool fresh = m_pageModel.rawWordsValid();
    const DeviceSnapshot &s = m_model.snapshot();
    m_comm[QStringLiteral("latency")]->setValue(
        fresh ? QString::number(s.dataAgeMs()) : QString(),
        QStringLiteral("ms"), fresh);
    m_comm[QStringLiteral("sequence")]->setValue(
        fresh ? QString::number(s.sequence()) : QString(), QString(), fresh);
    m_comm[QStringLiteral("reconnect")]->setValue(
        QString::number(m_pageModel.commStats().reconnectCount), QString(), true);
    m_comm[QStringLiteral("failedPolls")]->setValue(
        QString::number(m_pageModel.commStats().failedPolls), QString(), true);

    // Vision self-test (spec §7.4/§13): failure only marks this section red;
    // the rest of the page is unaffected.
    m_visionVersion->setText(m_pageModel.visionVersion().isEmpty()
                                 ? QStringLiteral("—")
                                 : m_pageModel.visionVersion());
    if (m_pageModel.visionVersion().isEmpty()) {
        m_visionStatus->setText(QStringLiteral("—"));
        m_visionFailure->setText(QStringLiteral("—"));
    } else if (m_pageModel.visionHealthy()) {
        m_visionStatus->setText(QStringLiteral("正常"));
        m_visionFailure->setText(QStringLiteral("—"));
    } else {
        m_visionStatus->setText(QStringLiteral("失败"));
        m_visionFailure->setText(m_pageModel.visionFailureReason().isEmpty()
                                     ? QStringLiteral("未知原因")
                                     : m_pageModel.visionFailureReason());
    }
}

} // namespace hlm
