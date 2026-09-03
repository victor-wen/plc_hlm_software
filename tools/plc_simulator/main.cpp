// plc_simulator.exe entry point (spec §14.3): standalone RTU simulator with
// the control panel. Owns the shared H3uSimulationModel; the ControlPanel
// owns the FaultInjector and the RtuServer (Task 18 review: the server holds
// a FaultInjector&, so the injector must outlive it).
//
// The simulator serves the shared model over a serial port (a Windows virtual
// COM pair in production) with an explicit station, so the production
// QtModbusPlcGateway can be exercised without a real PLC.

#include <QApplication>

#include "adapters/simulator/h3u_simulation_model.h"
#include "adapters/simulator/simulation_clock.h"
#include "tools/plc_simulator/control_panel.h"

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("plc_simulator"));
    app.setApplicationDisplayName(QStringLiteral("RTU 模拟器控制面板"));

    hlm::SimulationClock clock;
    hlm::H3uSimulationModel model(clock);
    hlm::ControlPanel panel(model);
    panel.setWindowTitle(QStringLiteral("RTU 模拟器控制面板"));
    panel.resize(900, 700);
    panel.show();

    return app.exec();
}
