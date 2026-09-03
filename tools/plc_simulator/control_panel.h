#pragma once

#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QWidget>

#include "tools/plc_simulator/fault_injection.h"
#include "tools/plc_simulator/rtu_server.h"

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QTableWidget;
class QTimer;

namespace hlm {

class H3uSimulationModel;
class RtuServer;

// Unit-testable core of the RTU simulator control panel (spec §14.3). Holds
// the connection configuration (port, baud, station), the scenario selection
// state and the request-log formatting. It only calls the backend interfaces
// (RtuServer::start/stop, FaultInjector setters, RtuRequestHandler::tick and
// requestLog) and reads the shared model for the register display — it never
// re-implements PLC business logic.
//
// The panel owns the FaultInjector and the RtuServer (Task 18 review: the
// server holds a FaultInjector&, so the injector must outlive it; the panel
// is the natural owner). The shared H3uSimulationModel is injected from the
// outside (the same model instance the RtuServer wraps).
class ControlPanelModel : public QObject
{
    Q_OBJECT

public:
    ControlPanelModel(RtuServer &server, FaultInjector &faults,
                      H3uSimulationModel &model, QObject *parent = nullptr);

    // --- connection configuration -------------------------------------------
    void setPortName(const QString &port);
    QString portName() const;
    void setBaudRate(int baud);
    int baudRate() const;
    void setStation(int station);
    int station() const;

    // Start/stop the RTU server. start() validates the configuration first
    // (spec §14.3: explicit station in 1-247; only 9600/19200 baud are
    // offered) and returns false with a human-readable statusText() on
    // rejection.
    bool start();
    void stop();
    bool isRunning() const;
    QString statusText() const;

    // --- scenario selection (only touches the FaultInjector) ----------------
    void setScenario(FaultInjector::Scenario scenario);
    FaultInjector::Scenario scenario() const;
    void setExceptionCode(quint8 code);
    void setDelayMs(int ms);
    void setFaultCode(int code);

    // --- register / coil state display ---------------------------------------
    quint16 registerValue(quint16 address) const;
    bool coilValue(quint16 address) const;

    // --- request log ----------------------------------------------------------
    // One human-readable line per logged request (功能码、地址、数量/值、是否
    // 响应、异常码).
    QStringList requestLogLines() const;
    void clearRequestLog();

    // --- simulation time -------------------------------------------------------
    // Advance the simulated PLC time by one second (drives D140, positioning,
    // home return and the M112 watchdog). The panel's QTimer calls this
    // periodically; tests call it directly for determinism.
    void tick();

private:
    RtuServer &m_server;
    FaultInjector &m_faults;
    H3uSimulationModel &m_model;

    QString m_portName;
    int m_baudRate = 9600;
    int m_station = 1;
    QString m_status = QStringLiteral("未连接");
};

// Qt Widgets control panel for the standalone RTU simulator (spec §14.3).
// Four groups: 连接 (port/baud/station/start/stop/status), 场景 (scenario
// selection + parameters), 状态 (key registers D100-D140 and M bits) and
// 请求日志 (readable request log, clearable). A QTimer drives
// handler().tick() every second so the simulated PLC time advances and the
// register display refreshes (Task 18 review: RtuServer never calls tick()).
//
// The panel owns the FaultInjector and the RtuServer; the shared model is
// injected (the same instance the RtuServer wraps).
class ControlPanel : public QWidget
{
    Q_OBJECT

public:
    explicit ControlPanel(H3uSimulationModel &model, QWidget *parent = nullptr);

    // --- test/inspection API ---------------------------------------------------
    QLineEdit *portEdit() const { return m_portEdit; }
    QComboBox *baudCombo() const { return m_baudCombo; }
    QSpinBox *stationSpin() const { return m_stationSpin; }
    QPushButton *startButton() const { return m_startButton; }
    QPushButton *stopButton() const { return m_stopButton; }
    QLabel *statusLabel() const { return m_statusLabel; }
    QComboBox *scenarioCombo() const { return m_scenarioCombo; }
    QSpinBox *exceptionSpin() const { return m_exceptionSpin; }
    QSpinBox *delaySpin() const { return m_delaySpin; }
    QSpinBox *faultCodeSpin() const { return m_faultCodeSpin; }
    QTableWidget *logTable() const { return m_logTable; }
    QPushButton *clearLogButton() const { return m_clearLogButton; }
    QLabel *registerLabel(const QString &key) const;

    RtuServer &server() { return m_server; }
    FaultInjector &faults() { return m_faults; }

public slots:
    // Re-renders every widget from the current state (called by the timer and
    // after every user action).
    void refresh();

private:
    void buildLayout();
    void onScenarioChanged(int index);
    void onStartClicked();
    void onStopClicked();
    void onClearLogClicked();
    void onTick();

    H3uSimulationModel &m_model;
    FaultInjector m_faults;
    RtuServer m_server;
    ControlPanelModel m_panelModel;
    QTimer *m_timer = nullptr;

    QLineEdit *m_portEdit = nullptr;
    QComboBox *m_baudCombo = nullptr;
    QSpinBox *m_stationSpin = nullptr;
    QPushButton *m_startButton = nullptr;
    QPushButton *m_stopButton = nullptr;
    QLabel *m_statusLabel = nullptr;
    QComboBox *m_scenarioCombo = nullptr;
    QSpinBox *m_exceptionSpin = nullptr;
    QSpinBox *m_delaySpin = nullptr;
    QSpinBox *m_faultCodeSpin = nullptr;
    QTableWidget *m_logTable = nullptr;
    QPushButton *m_clearLogButton = nullptr;
    QHash<QString, QLabel *> m_registerLabels;
};

} // namespace hlm
