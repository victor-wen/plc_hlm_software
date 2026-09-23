#pragma once

// Forward-program probe (user decision 2026-09-23: the HMI hands each board's
// barcodes to a local program, one argument per barcode).
//
// WHY THIS EXISTS: the forward step is the one part of the barcode adapter that
// cannot be proven with an injected fake — the whole question is "what does the
// REAL child process actually receive, and what does the adapter do with its
// exit code". The first version of these tests wrote a `#!/bin/sh` script and
// pointed the setting at it. That is green on the Linux dev loop and fails on
// Windows, which is the platform the canonical CI runs on and the platform this
// HMI ships to: `QProcess` starts a real image there, and a `.sh` file is not
// one. A `.bat` would be worse, not better — it goes through cmd.exe, which
// re-parses the command line and treats `^` as an escape, and `^` is a
// character the reference barcodes actually contain
// (需求/扫码相关/Barcode.txt), so the test would be measuring cmd.exe's
// quoting instead of this adapter's argument list.
//
// So the probe is the TEST BINARY ITSELF, re-entered with an environment
// variable set. A real executable on every platform, no shell anywhere, and the
// arguments the adapter builds arrive exactly as sent. The parent stays in
// charge of what the child does (exit code, how long to hang) through the
// environment, which QProcess inherits.
//
// Usage in a QTEST_MAIN test:
//
//   int main(int argc, char *argv[])
//   {
//       QApplication app(argc, argv);
//       if (hlm_test::forwardProbeRequested())
//           return hlm_test::runForwardProbe();
//       MyTest tc;
//       return QTest::qExec(&tc, argc, argv);
//   }
//
// and in a case:
//
//   hlm_test::ForwardProbe probe(dir);       // writes argv here
//   source.setForwardExePath(hlm_test::probeProgramPath());
//   ... run one cycle ...
//   QCOMPARE(probe.arguments(), QStringList({...}));

#include <QByteArray>
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QString>
#include <QStringList>
#include <QThread>

namespace hlm_test {

// Set by the test before starting a cycle. The child records its own argv here.
inline const char *kProbeOutEnv = "HLM_FORWARD_PROBE_OUT";
// Exit code the child returns (default 0). A non-zero code is a forward failure.
inline const char *kProbeExitEnv = "HLM_FORWARD_PROBE_EXIT";
// Milliseconds the child sleeps before exiting (default 0). Used to prove the
// adapter's timeout kills a hung program instead of waiting forever.
inline const char *kProbeSleepEnv = "HLM_FORWARD_PROBE_SLEEP_MS";
// Text the child prints on stdout before exiting. Used when the child stands in
// for the vendor's scanner CLI (trigger_client.exe), whose JSON reply is read
// from stdout rather than from a return buffer.
inline const char *kProbeStdoutEnv = "HLM_FORWARD_PROBE_STDOUT";

// True when this process was started as the forward program rather than as the
// test runner. Checked before any test object exists, so the child never runs
// a test case.
inline bool forwardProbeRequested()
{
    return !qEnvironmentVariable(kProbeOutEnv).isEmpty()
        || !qEnvironmentVariable(kProbeExitEnv).isEmpty()
        || !qEnvironmentVariable(kProbeSleepEnv).isEmpty()
        || !qEnvironmentVariable(kProbeStdoutEnv).isEmpty();
}

// The child's whole behaviour: record argv, optionally print a payload, hang,
// then exit with the requested code. Never returns to the test runner.
inline int runForwardProbe()
{
    const QString out = qEnvironmentVariable(kProbeOutEnv);
    if (!out.isEmpty()) {
        QFile file(out);
        if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            const QStringList arguments = QCoreApplication::arguments();
            // The count first, then one element per line, so a test proves both
            // the argument COUNT and the exact text of each one.
            file.write(QByteArray::number(arguments.size()) + "\n");
            for (const QString &argument : arguments)
                file.write(argument.toUtf8() + "\n");
        }
    }
    const QByteArray payload = qEnvironmentVariable(kProbeStdoutEnv).toUtf8();
    if (!payload.isEmpty()) {
        fwrite(payload.constData(), 1, size_t(payload.size()), stdout);
        fflush(stdout);
    }
    const int sleepMs = qEnvironmentVariableIntValue(kProbeSleepEnv);
    if (sleepMs > 0)
        QThread::msleep(static_cast<unsigned long>(sleepMs));
    return qEnvironmentVariableIntValue(kProbeExitEnv);
}

// The path the adapter must be pointed at: this very test executable.
inline QString probeProgramPath()
{
    return QCoreApplication::applicationFilePath();
}

// Set by probeProgramWithSpaceInPath() so the owning ForwardProbe can remove
// the copy again; the build tree is not a dumping ground.
inline QString &probeCopyPath()
{
    static QString path;
    return path;
}

// A copy of this executable whose PATH CONTAINS A SPACE, returned for the one
// case that must prove the adapter passes the program separately from its
// arguments (a shell-mediated invocation would break here). Removed by the
// ForwardProbe that owns the case.
//
// The copy is made NEXT TO the original, never in a temporary directory: on
// Windows the Qt DLLs are deployed app-local beside the test executable, so a
// copy moved anywhere else would fail to load its own dependencies and the test
// would be measuring a missing-DLL failure instead of the argument list.
inline QString probeProgramWithSpaceInPath()
{
    const QFileInfo self(QCoreApplication::applicationFilePath());
    const QString suffix = self.suffix().isEmpty()
        ? QString()
        : QLatin1Char('.') + self.suffix();
    const QString copy = self.absoluteDir().filePath(
        QStringLiteral("forward probe with space") + suffix);
    QFile::remove(copy); // a previous run may have left one behind
    if (QFile::copy(self.absoluteFilePath(), copy))
        probeCopyPath() = copy;
    return copy;
}

// The recorded argv of one probe run. `arguments()` drops argv[0] (the probe's
// own path, which the adapter does not choose); `rawCount()` is the count the
// child saw INCLUDING its own path, so a test can tell "the child never ran"
// from "the child ran with no arguments".
class ForwardProbe
{
public:
    explicit ForwardProbe(const QString &directory)
        : m_out(directory + QStringLiteral("/forward-probe-argv.txt"))
    {
        qputenv(kProbeOutEnv, m_out.toUtf8());
    }

    ~ForwardProbe()
    {
        qunsetenv(kProbeOutEnv);
        qunsetenv(kProbeExitEnv);
        qunsetenv(kProbeSleepEnv);
        if (!probeCopyPath().isEmpty()) {
            QFile::remove(probeCopyPath());
            probeCopyPath().clear();
        }
    }

    ForwardProbe(const ForwardProbe &) = delete;
    ForwardProbe &operator=(const ForwardProbe &) = delete;

    // Makes the next probe run exit with `code` instead of 0.
    void exitWith(int code) { qputenv(kProbeExitEnv, QByteArray::number(code)); }
    // Makes the next probe run hang for `ms` before exiting.
    void hangFor(int ms) { qputenv(kProbeSleepEnv, QByteArray::number(ms)); }

    QString outputPath() const { return m_out; }
    bool ran() const { return QFile::exists(m_out); }

    // Everything the child received after its own program path, in order.
    QStringList arguments() const
    {
        QStringList all = lines();
        if (!all.isEmpty())
            all.removeFirst(); // the probe's own path
        return all;
    }

private:
    QStringList lines() const
    {
        QFile file(m_out);
        if (!file.open(QIODevice::ReadOnly))
            return {};
        const QString text = QString::fromUtf8(file.readAll());
        file.close();
        QStringList lines = text.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        if (!lines.isEmpty())
            lines.removeFirst(); // the count line
        return lines;
    }

    QString m_out;
};

} // namespace hlm_test