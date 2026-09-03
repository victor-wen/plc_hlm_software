#include "tools/plc_simulator/control_panel.h"

#include <QComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHash>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

#include "adapters/simulator/h3u_simulation_model.h"
#include "tools/plc_simulator/rtu_server.h"

namespace hlm {

namespace {

// Key registers and coils shown in the 状态 group (spec §14.3: 当前寄存器状态).
constexpr struct {
    const char *key;
    const char *title;
    quint16 address;
    bool isCoil;
} kStateItems[] = {
    {"D100", "D100 状态字", 100, false},
    {"D110", "D110 故障代码", 110, false},
    {"D120", "D120 当前步骤", 120, false},
    {"D128", "D128 目标宽度", 128, false},
    {"D130", "D130 当前宽度", 130, false},
    {"D140", "D140 心跳", 140, false},
    {"M0", "M0 急停", 0, true},
    {"M14", "M14 锁存故障", 14, true},
    {"M34", "M34 调宽中", 34, true},
    {"M44", "M44 调宽成功", 44, true},
    {"M45", "M45 调宽失败", 45, true},
    {"M50", "M50 回原点", 50, true},
    {"M61", "M61 已回原点", 61, true},
};

// Scenario combo entries: display text -> FaultInjector::Scenario. The panel
// only maps the selection to the backend interface (spec §14.3).
constexpr struct {
    const char *text;
    FaultInjector::Scenario scenario;
} kScenarios[] = {
    {"成功 (正常)", FaultInjector::Scenario::None},
    {"条件失败", FaultInjector::Scenario::ConditionalFailure},
    {"超时", FaultInjector::Scenario::Timeout},
    {"故障代码", FaultInjector::Scenario::FaultCode},
    {"断线", FaultInjector::Scenario::Disconnect},
    {"延迟", FaultInjector::Scenario::Delay},
    {"异常响应", FaultInjector::Scenario::ExceptionResponse},
    {"非法值", FaultInjector::Scenario::IllegalValue},
    {"心跳冻结", FaultInjector::Scenario::HeartbeatFreeze},
};

constexpr int kMinTouchHeight = 48; // spec §11.1

QString functionCodeName(quint8 code)
{
    switch (code) {
    case 0x01:
        return QStringLiteral("读线圈");
    case 0x03:
        return QStringLiteral("读寄存器");
    case 0x05:
        return QStringLiteral("写线圈");
    case 0x06:
        return QStringLiteral("写寄存器");
    default:
        return QStringLiteral("功能码 0x%1").arg(code, 2, 16, QLatin1Char('0'));
    }
}

} // namespace

// --- ControlPanelModel ---------------------------------------------------------

ControlPanelModel::ControlPanelModel(RtuServer &server, FaultInjector &faults,
                                     H3uSimulationModel &model, QObject *parent)
    : QObject(parent)
    , m_server(server)
    , m_faults(faults)
    , m_model(model)
{
}

void ControlPanelModel::setPortName(const QString &port)
{
    m_portName = port.trimmed();
}

QString ControlPanelModel::portName() const
{
    return m_portName;
}

void ControlPanelModel::setBaudRate(int baud)
{
    m_baudRate = baud;
}

int ControlPanelModel::baudRate() const
{
    return m_baudRate;
}

void ControlPanelModel::setStation(int station)
{
    m_station = station;
}

int ControlPanelModel::station() const
{
    return m_station;
}

bool ControlPanelModel::start()
{
    // Spec §14.3: an explicit station in 1-247; 0/255 would answer broadcast
    // frames and mask addressing errors.
    if (m_station < 1 || m_station > 247) {
        m_status = QStringLiteral("启动失败: 站号必须在 1-247 之间");
        return false;
    }
    if (m_portName.isEmpty()) {
        m_status = QStringLiteral("启动失败: 串口不能为空");
        return false;
    }
    // Only 9600/19200 are offered by the panel (spec §14.3 波特率).
    if (m_baudRate != 9600 && m_baudRate != 19200) {
        m_status = QStringLiteral("启动失败: 波特率仅支持 9600/19200");
        return false;
    }

    if (!m_server.start(m_portName, m_baudRate, m_station)) {
        m_status = QStringLiteral("启动失败: 无法打开串口 %1")
                       .arg(m_portName);
        return false;
    }
    m_status = QStringLiteral("已连接: %1 @ %2, 站号 %3")
                   .arg(m_portName)
                   .arg(m_baudRate)
                   .arg(m_station);
    return true;
}

void ControlPanelModel::stop()
{
    m_server.stop();
    m_status = QStringLiteral("未连接");
}

bool ControlPanelModel::isRunning() const
{
    return m_server.isRunning();
}

QString ControlPanelModel::statusText() const
{
    return m_status;
}

void ControlPanelModel::setScenario(FaultInjector::Scenario scenario)
{
    m_faults.setScenario(scenario);
}

FaultInjector::Scenario ControlPanelModel::scenario() const
{
    return m_faults.scenario();
}

void ControlPanelModel::setExceptionCode(quint8 code)
{
    m_faults.setExceptionCode(code);
}

void ControlPanelModel::setDelayMs(int ms)
{
    m_faults.setDelayMs(ms);
}

void ControlPanelModel::setFaultCode(int code)
{
    m_faults.setFaultCode(code);
}

quint16 ControlPanelModel::registerValue(quint16 address) const
{
    return m_model.readRegister(address);
}

bool ControlPanelModel::coilValue(quint16 address) const
{
    return m_model.readCoil(address);
}

QStringList ControlPanelModel::requestLogLines() const
{
    QStringList lines;
    const auto &log = m_server.handler().requestLog();
    lines.reserve(log.size());
    for (const RtuRequestLogEntry &e : log) {
        const bool isRead = e.functionCode == 0x01 || e.functionCode == 0x03;
        const QString prefix =
            (e.functionCode == 0x01 || e.functionCode == 0x05)
            ? QStringLiteral("M")
            : QStringLiteral("D");
        const QString addrText = QStringLiteral("%1%2")
                                     .arg(prefix)
                                     .arg(e.address);
        QString line = QStringLiteral("%1 地址 %2")
                           .arg(functionCodeName(e.functionCode), addrText);
        if (isRead)
            line += QStringLiteral(" 数量 %1").arg(e.count);
        else
            line += QStringLiteral(" 值 %1").arg(e.value);
        if (e.exceptionCode != 0)
            line += QStringLiteral(" 异常 0x%1")
                        .arg(e.exceptionCode, 2, 16, QLatin1Char('0'));
        else if (e.responded)
            line += QStringLiteral(" 已响应");
        else
            line += QStringLiteral(" 未响应");
        lines.append(line);
    }
    return lines;
}

void ControlPanelModel::clearRequestLog()
{
    m_server.handler().clearRequestLog();
}

void ControlPanelModel::tick()
{
    m_server.handler().tick();
}

// --- ControlPanel --------------------------------------------------------------

ControlPanel::ControlPanel(H3uSimulationModel &model, QWidget *parent)
    : QWidget(parent)
    , m_model(model)
    , m_server(m_model, m_faults)
    , m_panelModel(m_server, m_faults, m_model)
{
    buildLayout();

    // Task 18 review: RtuServer never calls handler().tick(), so the panel
    // drives the simulated PLC time (D140 heartbeat, positioning, home return,
    // M112 watchdog) and refreshes the register display every second.
    m_timer = new QTimer(this);
    m_timer->setInterval(1000);
    connect(m_timer, &QTimer::timeout, this, &ControlPanel::onTick);
    m_timer->start();

    refresh();
}

void ControlPanel::buildLayout()
{
    auto *root = new QVBoxLayout(this);

    // --- 连接 group -----------------------------------------------------------
    auto *connBox = new QGroupBox(QStringLiteral("连接"), this);
    auto *connForm = new QFormLayout(connBox);
    m_portEdit = new QLineEdit(connBox);
    m_portEdit->setPlaceholderText(QStringLiteral("如 COM1 或 /dev/ttyS0"));
    m_baudCombo = new QComboBox(connBox);
    m_baudCombo->addItems({QStringLiteral("9600"), QStringLiteral("19200")});
    m_stationSpin = new QSpinBox(connBox);
    m_stationSpin->setRange(1, 247);
    m_stationSpin->setValue(1);
    connForm->addRow(QStringLiteral("串口"), m_portEdit);
    connForm->addRow(QStringLiteral("波特率"), m_baudCombo);
    connForm->addRow(QStringLiteral("站号"), m_stationSpin);

    auto *connButtons = new QHBoxLayout;
    m_startButton = new QPushButton(QStringLiteral("启动"), connBox);
    m_stopButton = new QPushButton(QStringLiteral("停止"), connBox);
    m_startButton->setMinimumHeight(kMinTouchHeight);
    m_stopButton->setMinimumHeight(kMinTouchHeight);
    connButtons->addWidget(m_startButton);
    connButtons->addWidget(m_stopButton);
    connForm->addRow(connButtons);

    m_statusLabel = new QLabel(connBox);
    m_statusLabel->setMinimumHeight(kMinTouchHeight);
    connForm->addRow(QStringLiteral("状态"), m_statusLabel);
    root->addWidget(connBox);

    // --- 场景 group -----------------------------------------------------------
    auto *scenarioBox = new QGroupBox(QStringLiteral("故障场景"), this);
    auto *scenarioForm = new QFormLayout(scenarioBox);
    m_scenarioCombo = new QComboBox(scenarioBox);
    for (const auto &s : kScenarios)
        m_scenarioCombo->addItem(QString::fromUtf8(s.text));
    m_exceptionSpin = new QSpinBox(scenarioBox);
    m_exceptionSpin->setRange(1, 255);
    m_exceptionSpin->setValue(1);
    m_delaySpin = new QSpinBox(scenarioBox);
    m_delaySpin->setRange(0, 10000);
    m_delaySpin->setSuffix(QStringLiteral(" ms"));
    m_faultCodeSpin = new QSpinBox(scenarioBox);
    m_faultCodeSpin->setRange(0, 10);
    m_faultCodeSpin->setValue(8);
    scenarioForm->addRow(QStringLiteral("场景"), m_scenarioCombo);
    scenarioForm->addRow(QStringLiteral("异常码"), m_exceptionSpin);
    scenarioForm->addRow(QStringLiteral("延迟"), m_delaySpin);
    scenarioForm->addRow(QStringLiteral("故障码"), m_faultCodeSpin);
    root->addWidget(scenarioBox);

    // --- 状态 group -----------------------------------------------------------
    auto *stateBox = new QGroupBox(QStringLiteral("寄存器状态"), this);
    auto *stateGrid = new QGridLayout(stateBox);
    int row = 0;
    int col = 0;
    for (const auto &item : kStateItems) {
        auto *title = new QLabel(QString::fromUtf8(item.title), stateBox);
        auto *value = new QLabel(QStringLiteral("—"), stateBox);
        value->setMinimumHeight(kMinTouchHeight);
        m_registerLabels.insert(QString::fromUtf8(item.key), value);
        stateGrid->addWidget(title, row, col * 2);
        stateGrid->addWidget(value, row, col * 2 + 1);
        ++row;
        if (row == 7) {
            row = 0;
            ++col;
        }
    }
    root->addWidget(stateBox);

    // --- 请求日志 group ---------------------------------------------------------
    auto *logBox = new QGroupBox(QStringLiteral("请求日志"), this);
    auto *logLayout = new QVBoxLayout(logBox);
    m_logTable = new QTableWidget(logBox);
    m_logTable->setColumnCount(6);
    m_logTable->setHorizontalHeaderLabels(
        {QStringLiteral("功能码"), QStringLiteral("地址"), QStringLiteral("数量"),
         QStringLiteral("值"), QStringLiteral("响应"), QStringLiteral("异常码")});
    m_logTable->horizontalHeader()->setStretchLastSection(true);
    m_logTable->verticalHeader()->setDefaultSectionSize(kMinTouchHeight);
    m_logTable->verticalHeader()->setMinimumSectionSize(kMinTouchHeight);
    m_logTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_logTable->setSelectionMode(QAbstractItemView::NoSelection);
    m_clearLogButton = new QPushButton(QStringLiteral("清空日志"), logBox);
    m_clearLogButton->setMinimumHeight(kMinTouchHeight);
    logLayout->addWidget(m_logTable);
    logLayout->addWidget(m_clearLogButton);
    root->addWidget(logBox);

    connect(m_startButton, &QPushButton::clicked, this, &ControlPanel::onStartClicked);
    connect(m_stopButton, &QPushButton::clicked, this, &ControlPanel::onStopClicked);
    connect(m_clearLogButton, &QPushButton::clicked, this,
            &ControlPanel::onClearLogClicked);
    connect(m_scenarioCombo, &QComboBox::currentIndexChanged, this,
            &ControlPanel::onScenarioChanged);
    // Scenario parameter spinboxes push their values into the FaultInjector
    // immediately (Task 19 review: they were dead UI otherwise).
    connect(m_exceptionSpin, &QSpinBox::valueChanged, this,
            [this](int v) { m_panelModel.setExceptionCode(quint8(v)); });
    connect(m_delaySpin, &QSpinBox::valueChanged, this,
            [this](int v) { m_panelModel.setDelayMs(v); });
    connect(m_faultCodeSpin, &QSpinBox::valueChanged, this,
            [this](int v) { m_panelModel.setFaultCode(v); });
}

QLabel *ControlPanel::registerLabel(const QString &key) const
{
    return m_registerLabels.value(key);
}

void ControlPanel::refresh()
{
    // Connection status.
    m_statusLabel->setText(m_panelModel.statusText());

    // Register / coil state.
    for (const auto &item : kStateItems) {
        QLabel *label = m_registerLabels.value(QString::fromUtf8(item.key));
        if (!label)
            continue;
        if (item.isCoil)
            label->setText(m_model.readCoil(item.address) ? QStringLiteral("1")
                                                          : QStringLiteral("0"));
        else
            label->setText(QString::number(m_model.readRegister(item.address)));
    }

    // Request log.
    const auto &log = m_server.handler().requestLog();
    m_logTable->setRowCount(log.size());
    for (int i = 0; i < log.size(); ++i) {
        const RtuRequestLogEntry &e = log[i];
        auto *fc = new QTableWidgetItem(
            QStringLiteral("%1").arg(e.functionCode, 2, 16, QLatin1Char('0')));
        auto *addr = new QTableWidgetItem(
            QStringLiteral("%1%2")
                .arg((e.functionCode == 0x01 || e.functionCode == 0x05)
                         ? QStringLiteral("M")
                         : QStringLiteral("D"))
                .arg(e.address));
        auto *count = new QTableWidgetItem(
            e.count ? QString::number(e.count) : QString());
        auto *value = new QTableWidgetItem(
            (e.functionCode == 0x05 || e.functionCode == 0x06)
                ? QString::number(e.value)
                : QString());
        auto *resp = new QTableWidgetItem(
            e.exceptionCode != 0 ? QStringLiteral("异常")
                                 : (e.responded ? QStringLiteral("已响应")
                                                : QStringLiteral("未响应")));
        auto *exc = new QTableWidgetItem(
            e.exceptionCode != 0
                ? QStringLiteral("0x%1")
                      .arg(e.exceptionCode, 2, 16, QLatin1Char('0'))
                : QString());
        m_logTable->setItem(i, 0, fc);
        m_logTable->setItem(i, 1, addr);
        m_logTable->setItem(i, 2, count);
        m_logTable->setItem(i, 3, value);
        m_logTable->setItem(i, 4, resp);
        m_logTable->setItem(i, 5, exc);
    }
}

void ControlPanel::onScenarioChanged(int index)
{
    if (index < 0 || index >= int(std::size(kScenarios)))
        return;
    const FaultInjector::Scenario scenario = kScenarios[index].scenario;
    // Push the current spinbox values so pre-filled parameters apply to the
    // newly selected scenario (Task 19 review: values were never forwarded).
    m_panelModel.setExceptionCode(quint8(m_exceptionSpin->value()));
    m_panelModel.setDelayMs(m_delaySpin->value());
    m_panelModel.setFaultCode(m_faultCodeSpin->value());
    m_panelModel.setScenario(scenario);
    // Each parameter widget is only meaningful for its own scenario; the
    // others are disabled to avoid implying they apply.
    m_exceptionSpin->setEnabled(scenario == FaultInjector::Scenario::ExceptionResponse);
    m_delaySpin->setEnabled(scenario == FaultInjector::Scenario::Delay);
    m_faultCodeSpin->setEnabled(scenario == FaultInjector::Scenario::FaultCode);
}

void ControlPanel::onStartClicked()
{
    m_panelModel.setPortName(m_portEdit->text());
    m_panelModel.setBaudRate(m_baudCombo->currentText().toInt());
    m_panelModel.setStation(m_stationSpin->value());
    m_panelModel.start();
    refresh();
}

void ControlPanel::onStopClicked()
{
    m_panelModel.stop();
    refresh();
}

void ControlPanel::onClearLogClicked()
{
    m_panelModel.clearRequestLog();
    refresh();
}

void ControlPanel::onTick()
{
    // Advance the simulated PLC time and refresh the register display.
    m_panelModel.tick();
    refresh();
}

} // namespace hlm
