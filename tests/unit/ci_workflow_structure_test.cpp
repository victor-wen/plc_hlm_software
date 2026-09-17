// CI-CACHE-001 black-box static contract checks for the windows-build-test
// vcpkg binary-cache toolchain identity.
//
// Authored only from .ai/test-briefs/CI-CACHE-001.yaml (brief_version 1), the
// approved .ai/project-contract.yaml, and the workflow file itself. No
// production implementation source, git history, or change diff was read.
//
// Deterministic and offline: the workflow is read as text and checked with
// structural parsing only. No network, runner, YAML engine, or PowerShell
// execution is involved.
//
// Expected RED before the fix (current workflow):
//   - no capture step references $env:ImageVersion or hashes the compiler
//     binary at $env:VCToolsInstallDir\bin\HostX64\x64\cl.exe,
//   - the broad restore key "vcpkg-binary-v1-${{ runner.os }}-x64-" is present
//     in all three cache steps,
//   - the top-level "on:" block has no workflow_dispatch trigger.
//
// Brief assertion traceability:
//   OB-1: exactlyThreeCaptureStepsReferenceTheRunnerImageVersion,
//         everyCaptureStepHashesTheCompilerBinaryAtTheVCToolsInstallDir,
//         everyCaptureStepEmitsAnIdContainingCompilerVersionAndSdkParts
//   OB-2: exactlyThreeCacheStepsUseTheVcpkgBinaryKeyPrefix,
//         everyCacheKeyCarriesRunnerOsAndTheToolchainOutputId,
//         everyCacheKeyCarriesTheVcpkgManifestHash,
//         noCacheStepRestoreKeyLacksTheToolchainIdentitySegment,
//         theThreeCacheStepsShareIdenticalKeyAndRestoreKeyExpressions
//   OB-3: workflowDispatchTriggerIsPresent,
//         pushAndPullRequestTriggersRemainPresent
//   OB-4: jobKeysRemainExactlyBuildPackageVisionOff,
//         recordedActionPinsRemainPresent,
//         canonicalCtestAndDeployCheckInvocationsRemainPresent
//   OB-5: idExpressionChangesWhenOnlyTheRunnerImageVersionDiffers,
//         noRestoreKeyCanMatchADifferentToolchainIdentity
//   Boundaries: captureScriptKeepsFallbacksForMissingImageVersionAndCompilerBinary
//   Verify-phase additions (CI-CACHE-001 VERIFY):
//         theThreeCaptureStepsShareTextuallyIdenticalIdentityScripts (boundary:
//           the capture scripts must not diverge between jobs),
//         theThreeCacheStepsKeepExactlyOneIdentityRestoreKeyEach (OB-2/OB-5:
//           exactly one identity-bearing restore key, no second fallback).

#include <QtTest>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

namespace {

const QString kWorkflowRelativePath =
    QStringLiteral(".github/workflows/windows-build-test.yml");
const QString kToolchainStepId = QStringLiteral("toolchain");
const QString kIdentityOutputToken = QStringLiteral("steps.toolchain.outputs.id");
const QString kImageVariable = QStringLiteral("$env:ImageVersion");
const QString kCompilerVersionVariable = QStringLiteral("$env:VCToolsVersion");
const QString kSdkVariable = QStringLiteral("$env:WindowsSDKVersion");
const QString kClExeWindowsPath = QStringLiteral("bin\\HostX64\\x64\\cl.exe");
const QString kClExePosixPath = QStringLiteral("bin/HostX64/x64/cl.exe");
const QString kLegacyBroadRestoreKey =
    QStringLiteral("vcpkg-binary-v1-${{ runner.os }}-x64-");

struct WorkflowStep
{
    QString name;
    QString id;
    QString uses;
    QString key;
    QString run;
    QString ifCondition;
    QStringList runLines;
    QStringList restoreKeys;
    bool hasRestoreKeys = false;
    QString job;
};

struct WorkflowFile
{
    QString path;
    QString text;
    QStringList lines;
    QStringList jobKeys;
    QStringList onBlockLines;
    QVector<WorkflowStep> steps;
    QString parseError;
};

int lineIndent(const QString &line)
{
    int indent = 0;
    while (indent < line.size()
           && (line.at(indent) == QLatin1Char(' ') || line.at(indent) == QLatin1Char('\t'))) {
        ++indent;
    }
    return indent;
}

QString unquote(QString value)
{
    value = value.trimmed();
    if (value.size() >= 2
        && ((value.startsWith(QLatin1Char('"')) && value.endsWith(QLatin1Char('"')))
            || (value.startsWith(QLatin1Char('\'')) && value.endsWith(QLatin1Char('\''))))) {
        return value.mid(1, value.size() - 2);
    }
    // Strip a trailing YAML comment from unquoted scalars (e.g. action pins).
    const int comment = value.indexOf(QStringLiteral(" #"));
    if (comment >= 0)
        value = value.left(comment).trimmed();
    return value;
}

QStringList topLevelBlock(const QStringList &lines, const QString &key)
{
    static const QRegularExpression topLevelRe(
        QStringLiteral("^([A-Za-z_][A-Za-z0-9_.-]*):(.*)$"));
    qsizetype start = -1;
    for (qsizetype i = 0; i < lines.size(); ++i) {
        const QRegularExpressionMatch match = topLevelRe.match(lines.at(i));
        if (match.hasMatch() && match.captured(1) == key) {
            start = i;
            break;
        }
    }
    QStringList block;
    if (start < 0)
        return block;
    for (qsizetype i = start + 1; i < lines.size(); ++i) {
        const QString &line = lines.at(i);
        if (!line.trimmed().isEmpty() && !line.trimmed().startsWith(QLatin1Char('#'))
            && lineIndent(line) == 0) {
            break;
        }
        block << line;
    }
    return block;
}

QStringList nestedBlock(const QStringList &lines, const QString &key)
{
    const QRegularExpression startRe(
        QStringLiteral("^(\\s*)%1\\s*:\\s*$").arg(QRegularExpression::escape(key)));
    qsizetype start = -1;
    int indent = 0;
    for (qsizetype i = 0; i < lines.size(); ++i) {
        const QRegularExpressionMatch match = startRe.match(lines.at(i));
        if (match.hasMatch()) {
            start = i;
            indent = int(match.captured(1).size());
            break;
        }
    }
    QStringList block;
    if (start < 0)
        return block;
    for (qsizetype i = start + 1; i < lines.size(); ++i) {
        const QString &line = lines.at(i);
        if (line.trimmed().isEmpty() || line.trimmed().startsWith(QLatin1Char('#')))
            continue;
        if (lineIndent(line) <= indent)
            break;
        block << line;
    }
    return block;
}

QStringList parseJobKeys(const QStringList &lines)
{
    static const QRegularExpression jobKeyRe(
        QStringLiteral("^  ([A-Za-z0-9_.][A-Za-z0-9_.-]*):\\s*$"));
    QStringList keys;
    // Scoped to the top-level jobs: block so trigger keys in on: cannot match.
    const QStringList block = topLevelBlock(lines, QStringLiteral("jobs"));
    for (const QString &line : block) {
        const QRegularExpressionMatch match = jobKeyRe.match(line);
        if (match.hasMatch())
            keys << match.captured(1);
    }
    return keys;
}

void assignStepKey(WorkflowStep &step, const QString &key, const QString &value)
{
    if (key == QLatin1String("name"))
        step.name = unquote(value);
    else if (key == QLatin1String("id"))
        step.id = unquote(value);
    else if (key == QLatin1String("uses"))
        step.uses = unquote(value);
    else if (key == QLatin1String("key"))
        step.key = unquote(value);
}

WorkflowStep parseStepBlock(const QStringList &block, int stepIndent, const QString &job)
{
    WorkflowStep step;
    step.job = job;
    if (block.isEmpty())
        return step;

    static const QRegularExpression firstLineRe(
        QStringLiteral("^\\s*-\\s+([A-Za-z0-9_-]+):\\s*(.*)$"));
    const QRegularExpressionMatch firstMatch = firstLineRe.match(block.first());
    if (firstMatch.hasMatch())
        assignStepKey(step, firstMatch.captured(1), firstMatch.captured(2));

    static const QRegularExpression keyValueRe(
        QStringLiteral("^(\\s*)([A-Za-z0-9_.-]+):\\s?(.*)$"));
    qsizetype i = 1;
    while (i < block.size()) {
        const QString &line = block.at(i);
        const QRegularExpressionMatch match = keyValueRe.match(line);
        if (!match.hasMatch()) {
            ++i;
            continue;
        }
        const int keyIndent = int(match.captured(1).size());
        const QString key = match.captured(2);
        const QString value = match.captured(3).trimmed();
        const bool blockScalar = value.isEmpty() || value.startsWith(QLatin1Char('|'))
                                 || value.startsWith(QLatin1Char('>'));

        if (key == QLatin1String("run") || key == QLatin1String("restore-keys")) {
            if (blockScalar) {
                QStringList collected;
                qsizetype j = i + 1;
                while (j < block.size()) {
                    const QString &sub = block.at(j);
                    if (sub.trimmed().isEmpty()) {
                        ++j;
                        continue;
                    }
                    if (lineIndent(sub) <= keyIndent)
                        break;
                    collected << sub.trimmed();
                    ++j;
                }
                if (key == QLatin1String("run")) {
                    step.runLines = collected;
                    step.run = collected.join(QLatin1Char('\n'));
                } else {
                    step.hasRestoreKeys = true;
                    step.restoreKeys = collected;
                }
                i = j;
                continue;
            }
            if (key == QLatin1String("run")) {
                step.runLines = QStringList{ unquote(value) };
                step.run = unquote(value);
            } else {
                step.hasRestoreKeys = true;
                step.restoreKeys = QStringList{ unquote(value) };
            }
            ++i;
            continue;
        }

        if (keyIndent == stepIndent + 2
            && (key == QLatin1String("name") || key == QLatin1String("id")
                || key == QLatin1String("uses"))) {
            assignStepKey(step, key, value);
        } else if (key == QLatin1String("key")) {
            step.key = unquote(value);
        } else if (key == QLatin1String("if") && keyIndent == stepIndent + 2) {
            step.ifCondition = unquote(value);
        }
        ++i;
    }
    return step;
}

QVector<WorkflowStep> parseSteps(const QStringList &lines)
{
    static const QRegularExpression jobKeyRe(
        QStringLiteral("^  ([A-Za-z0-9_.][A-Za-z0-9_.-]*):\\s*$"));
    static const QRegularExpression stepRe(
        QStringLiteral("^(\\s*)-\\s+[A-Za-z0-9_-]+:.*$"));

    struct StepStart
    {
        qsizetype index = 0;
        int indent = 0;
        QString job;
    };
    QVector<StepStart> starts;
    QString currentJob;
    for (qsizetype i = 0; i < lines.size(); ++i) {
        const QRegularExpressionMatch jobMatch = jobKeyRe.match(lines.at(i));
        if (jobMatch.hasMatch()) {
            currentJob = jobMatch.captured(1);
            continue;
        }
        const QRegularExpressionMatch stepMatch = stepRe.match(lines.at(i));
        if (stepMatch.hasMatch())
            starts.append({ i, int(stepMatch.captured(1).size()), currentJob });
    }

    QVector<WorkflowStep> steps;
    for (qsizetype s = 0; s < starts.size(); ++s) {
        qsizetype end = lines.size();
        for (qsizetype n = s + 1; n < starts.size(); ++n) {
            if (starts.at(n).indent <= starts.at(s).indent) {
                end = starts.at(n).index;
                break;
            }
        }
        for (qsizetype i = starts.at(s).index + 1; i < end; ++i) {
            const QString &line = lines.at(i);
            if (line.trimmed().isEmpty() || line.trimmed().startsWith(QLatin1Char('#')))
                continue;
            if (lineIndent(line) < starts.at(s).indent) {
                end = i;
                break;
            }
        }
        const QStringList block = lines.mid(starts.at(s).index, end - starts.at(s).index);
        WorkflowStep step = parseStepBlock(block, starts.at(s).indent, starts.at(s).job);
        steps.append(step);
    }
    return steps;
}

QString locateWorkflowFile()
{
    const QString foundByTestData =
        QFINDTESTDATA("../../.github/workflows/windows-build-test.yml");
    if (!foundByTestData.isEmpty() && QFileInfo::exists(foundByTestData))
        return QDir::cleanPath(foundByTestData);

    const QStringList roots = { QDir::currentPath(), QCoreApplication::applicationDirPath() };
    for (const QString &root : roots) {
        QDir dir(root);
        for (int depth = 0; depth < 12; ++depth) {
            const QString candidate = dir.filePath(kWorkflowRelativePath);
            if (QFileInfo::exists(candidate))
                return QDir::cleanPath(candidate);
            if (!dir.cdUp())
                break;
        }
    }
    return QString();
}

bool loadWorkflow(WorkflowFile &workflow)
{
    workflow.path = locateWorkflowFile();
    if (workflow.path.isEmpty()) {
        workflow.parseError = QStringLiteral(
            "workflow file %1 was not found relative to the test source, the current "
            "directory, or the test executable; the check must fail, not skip")
            .arg(kWorkflowRelativePath);
        return false;
    }

    QFile file(workflow.path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        workflow.parseError = QStringLiteral("workflow file '%1' cannot be read: %2")
                                  .arg(workflow.path, file.errorString());
        return false;
    }
    workflow.text = QString::fromUtf8(file.readAll());
    workflow.text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    workflow.text.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    workflow.lines = workflow.text.split(QLatin1Char('\n'));

    workflow.jobKeys = parseJobKeys(workflow.lines);
    workflow.onBlockLines = topLevelBlock(workflow.lines, QStringLiteral("on"));
    workflow.steps = parseSteps(workflow.lines);

    if (workflow.jobKeys.isEmpty() || workflow.onBlockLines.isEmpty()
        || workflow.steps.isEmpty()) {
        workflow.parseError = QStringLiteral(
            "workflow file '%1' is not parsable: jobs, on, or steps could not be "
            "located; the check must fail, not skip")
            .arg(workflow.path);
        return false;
    }
    return true;
}

QVector<WorkflowStep> stepsUsingAction(const QVector<WorkflowStep> &steps,
                                       const QString &actionPrefix)
{
    QVector<WorkflowStep> result;
    for (const WorkflowStep &step : steps) {
        if (step.uses.startsWith(actionPrefix))
            result.append(step);
    }
    return result;
}

QVector<WorkflowStep> captureStepsIn(const QVector<WorkflowStep> &steps)
{
    QVector<WorkflowStep> result;
    for (const WorkflowStep &step : steps) {
        if (step.run.contains(kImageVariable))
            result.append(step);
    }
    return result;
}

QStringList emissionLines(const WorkflowStep &step)
{
    QStringList lines;
    for (const QString &line : step.runLines) {
        if (line.contains(QLatin1String("id=")))
            lines << line;
    }
    return lines;
}

// Returns the token whose substitution changes the emitted step id; either
// $env:ImageVersion used directly in the id emission, or a variable bound from
// $env:ImageVersion and used in the id emission. Empty means the emitted id
// cannot change when the runner image changes.
QString imageTokenInEmission(const WorkflowStep &step)
{
    const QStringList emissions = emissionLines(step);
    for (const QString &line : emissions) {
        if (line.contains(kImageVariable))
            return kImageVariable;
    }

    static const QRegularExpression bindingRe(
        QStringLiteral("\\$(\\w+)\\s*=[^\\n]*\\$env:ImageVersion"));
    QSet<QString> boundTokens;
    for (const QString &line : step.runLines) {
        const QRegularExpressionMatch match = bindingRe.match(line);
        if (match.hasMatch())
            boundTokens.insert(QStringLiteral("$") + match.captured(1));
    }
    for (const QString &token : boundTokens) {
        for (const QString &line : emissions) {
            if (line.contains(token))
                return token;
        }
    }
    return QString();
}

bool mentionsCompilerBinary(const QString &line)
{
    return line.contains(kClExeWindowsPath, Qt::CaseInsensitive)
           || line.contains(kClExePosixPath, Qt::CaseInsensitive);
}

bool hashesCompilerBinary(const WorkflowStep &step)
{
    static const QRegularExpression hashRe(
        QStringLiteral("Get-FileHash|SHA256|HashData|hashFiles"),
        QRegularExpression::CaseInsensitiveOption);

    for (const QString &line : step.runLines) {
        if (mentionsCompilerBinary(line) && hashRe.match(line).hasMatch())
            return true;
    }

    static const QRegularExpression bindingRe(
        QStringLiteral("\\$(\\w+)\\s*=[^\\n]*(cl\\.exe|VCToolsInstallDir)"),
        QRegularExpression::CaseInsensitiveOption);
    QSet<QString> boundTokens;
    for (const QString &line : step.runLines) {
        const QRegularExpressionMatch match = bindingRe.match(line);
        if (match.hasMatch())
            boundTokens.insert(QStringLiteral("$") + match.captured(1));
    }
    for (const QString &token : boundTokens) {
        for (const QString &line : step.runLines) {
            if (line.contains(token) && hashRe.match(line).hasMatch())
                return true;
        }
    }
    return false;
}

bool containsGuard(const WorkflowStep &step, const QRegularExpression &guardRe)
{
    for (const QString &line : step.runLines) {
        if (guardRe.match(line).hasMatch())
            return true;
    }
    return false;
}

bool bindingLineFor(const WorkflowStep &step, const QString &token, QString &line)
{
    const QRegularExpression bindingRe(
        QStringLiteral("^\\s*%1\\s*=").arg(QRegularExpression::escape(token)));
    for (const QString &candidate : step.runLines) {
        if (bindingRe.match(candidate).hasMatch()) {
            line = candidate;
            return true;
        }
    }
    return false;
}

// Boundary: an empty $env:ImageVersion must still leave a usable id token, so
// the image value that reaches the id emission must be fallback-guarded (either
// the emission line itself, or the binding of the variable it uses).
bool imageValueInIdEmissionIsFallbackGuarded(const WorkflowStep &step)
{
    static const QRegularExpression fallbackRe(
        QStringLiteral("\\bif\\b|\\belse\\b|\\?\\?|-or\\s|-ne\\s|IsNullOrEmpty|"
                       "IsNullOrWhiteSpace|\\bswitch\\b|\\bdefault\\b"));
    const QString token = imageTokenInEmission(step);
    if (token.isEmpty())
        return false;

    const QStringList emissions = emissionLines(step);
    for (const QString &line : emissions) {
        if (line.contains(token) && fallbackRe.match(line).hasMatch())
            return true;
    }
    if (token == kImageVariable)
        return false; // emitted directly and unguarded

    QString binding;
    if (bindingLineFor(step, token, binding) && fallbackRe.match(binding).hasMatch())
        return true;
    return false;
}

} // namespace

class CiWorkflowStructureTest : public QObject
{
    Q_OBJECT

private slots:
    void workflowFileIsPresentAndParsable()
    {
        WorkflowFile workflow;
        QVERIFY2(loadWorkflow(workflow), qPrintable(workflow.parseError));
        QVERIFY2(QFileInfo(workflow.path).size() > 0,
                 "the workflow file is empty; the structural check must fail, not pass");
    }

    void exactlyThreeCaptureStepsReferenceTheRunnerImageVersion()
    {
        WorkflowFile workflow;
        QVERIFY2(loadWorkflow(workflow), qPrintable(workflow.parseError));

        // Primary property: the capture step's run script references
        // $env:ImageVersion. Every step with a run script is examined so the
        // failure names the missing property explicitly (not skip, not pass).
        int captureSteps = 0;
        for (const WorkflowStep &step : workflow.steps) {
            if (step.run.isEmpty())
                continue;
            if (step.run.contains(kImageVariable))
                ++captureSteps;
        }
        QVERIFY2(captureSteps == 3,
                 qPrintable(QStringLiteral(
                                "expected exactly 3 toolchain identity capture steps whose run "
                                "script references $env:ImageVersion (one per job: build, "
                                "package, vision-off); observed %1. The capture step's run "
                                "script must reference the runner image version environment "
                                "variable ($env:ImageVersion)")
                                .arg(captureSteps)));

        const QVector<WorkflowStep> captures = captureStepsIn(workflow.steps);
        QSet<QString> jobs;
        for (const WorkflowStep &step : captures)
            jobs.insert(step.job);
        QVERIFY2(jobs.size() == 3,
                 qPrintable(QStringLiteral(
                                "expected one capture step per job (3 distinct jobs); "
                                "observed %1 distinct job(s): %2")
                                .arg(jobs.size())
                                .arg(QStringList(jobs.constBegin(), jobs.constEnd())
                                         .join(QStringLiteral(", ")))));

        for (const WorkflowStep &step : captures) {
            QVERIFY2(step.id == kToolchainStepId,
                     qPrintable(QStringLiteral(
                                    "capture step '%1' in job '%2' must declare id: %3 so the "
                                    "cache keys can reference steps.%3.outputs.id; observed "
                                    "id '%4'")
                                    .arg(step.name, step.job, kToolchainStepId, step.id)));
        }
    }

    void everyCaptureStepHashesTheCompilerBinaryAtTheVCToolsInstallDir()
    {
        WorkflowFile workflow;
        QVERIFY2(loadWorkflow(workflow), qPrintable(workflow.parseError));

        const QVector<WorkflowStep> captures = captureStepsIn(workflow.steps);
        QVERIFY2(captures.size() == 3,
                 qPrintable(QStringLiteral(
                                "expected exactly 3 capture steps referencing $env:ImageVersion "
                                "before checking the compiler binary hash; observed %1")
                                .arg(captures.size())));

        for (const WorkflowStep &step : captures) {
            QVERIFY2(step.run.contains(QStringLiteral("$env:VCToolsInstallDir")),
                     qPrintable(QStringLiteral(
                                    "capture step '%1' in job '%2' does not reference "
                                    "$env:VCToolsInstallDir")
                                    .arg(step.name, step.job)));
            QVERIFY2(hashesCompilerBinary(step),
                     qPrintable(QStringLiteral(
                                    "capture step '%1' in job '%2' does not hash the compiler "
                                    "binary at $env:VCToolsInstallDir\\bin\\HostX64\\x64\\cl.exe "
                                    "(a hash primitive must be applied to that path)")
                                    .arg(step.name, step.job)));
        }
    }

    void everyCaptureStepEmitsAnIdContainingCompilerVersionAndSdkParts()
    {
        WorkflowFile workflow;
        QVERIFY2(loadWorkflow(workflow), qPrintable(workflow.parseError));

        const QVector<WorkflowStep> captures = captureStepsIn(workflow.steps);
        QVERIFY2(captures.size() == 3,
                 qPrintable(QStringLiteral(
                                "expected exactly 3 capture steps before checking the emitted "
                                "id parts; observed %1")
                                .arg(captures.size())));

        for (const WorkflowStep &step : captures) {
            QVERIFY2(emissionLines(step).size() >= 1,
                     qPrintable(QStringLiteral(
                                    "capture step '%1' in job '%2' does not emit a step output "
                                    "id (no GITHUB_OUTPUT id= emission found)")
                                    .arg(step.name, step.job)));
            QVERIFY2(step.run.contains(kCompilerVersionVariable),
                     qPrintable(QStringLiteral(
                                    "capture step '%1' in job '%2': the emitted id lost the "
                                    "compiler version part ($env:VCToolsVersion)")
                                    .arg(step.name, step.job)));
            QVERIFY2(step.run.contains(kSdkVariable) || step.run.contains(QStringLiteral("$sdk")),
                     qPrintable(QStringLiteral(
                                    "capture step '%1' in job '%2': the emitted id lost the "
                                    "SDK part ($env:WindowsSDKVersion)")
                                    .arg(step.name, step.job)));
        }
    }

    void exactlyThreeCacheStepsUseTheVcpkgBinaryKeyPrefix()
    {
        WorkflowFile workflow;
        QVERIFY2(loadWorkflow(workflow), qPrintable(workflow.parseError));

        const QVector<WorkflowStep> caches =
            stepsUsingAction(workflow.steps, QStringLiteral("actions/cache@"));
        QVERIFY2(caches.size() == 3,
                 qPrintable(QStringLiteral(
                                "expected exactly 3 steps using actions/cache; observed %1")
                                .arg(caches.size())));

        int withPrefix = 0;
        for (const WorkflowStep &step : caches) {
            if (step.key.contains(QStringLiteral("vcpkg-binary-v1-")))
                ++withPrefix;
        }
        QVERIFY2(withPrefix == 3,
                 qPrintable(QStringLiteral(
                                "expected exactly 3 cache steps whose key contains "
                                "'vcpkg-binary-v1-'; observed %1")
                                .arg(withPrefix)));

        QSet<QString> jobs;
        for (const WorkflowStep &step : caches)
            jobs.insert(step.job);
        QVERIFY2(jobs.size() == 3,
                 qPrintable(QStringLiteral(
                                "expected one vcpkg binary cache step per job (3 distinct "
                                "jobs); observed %1")
                                .arg(jobs.size())));
    }

    void everyCacheKeyCarriesRunnerOsAndTheToolchainOutputId()
    {
        WorkflowFile workflow;
        QVERIFY2(loadWorkflow(workflow), qPrintable(workflow.parseError));

        const QVector<WorkflowStep> caches =
            stepsUsingAction(workflow.steps, QStringLiteral("actions/cache@"));
        QVERIFY2(caches.size() == 3,
                 qPrintable(QStringLiteral("expected 3 cache steps; observed %1")
                                .arg(caches.size())));

        for (const WorkflowStep &step : caches) {
            QVERIFY2(step.key.contains(QStringLiteral("runner.os")),
                     qPrintable(QStringLiteral(
                                    "cache step '%1' in job '%2': key lost the runner.os part")
                                    .arg(step.name, step.job)));
            QVERIFY2(step.key.contains(kIdentityOutputToken),
                     qPrintable(QStringLiteral(
                                    "cache step '%1' in job '%2': key does not contain '%3', "
                                    "so a runner image change cannot change the cache key")
                                    .arg(step.name, step.job, kIdentityOutputToken)));
        }
    }

    void everyCacheKeyCarriesTheVcpkgManifestHash()
    {
        WorkflowFile workflow;
        QVERIFY2(loadWorkflow(workflow), qPrintable(workflow.parseError));

        const QVector<WorkflowStep> caches =
            stepsUsingAction(workflow.steps, QStringLiteral("actions/cache@"));
        QVERIFY2(caches.size() == 3,
                 qPrintable(QStringLiteral("expected 3 cache steps; observed %1")
                                .arg(caches.size())));

        for (const WorkflowStep &step : caches) {
            QVERIFY2(step.key.contains(QStringLiteral("hashFiles('vcpkg.json')")),
                     qPrintable(QStringLiteral(
                                    "cache step '%1' in job '%2': key does not contain "
                                    "hashFiles('vcpkg.json')")
                                    .arg(step.name, step.job)));
        }
    }

    void noCacheStepRestoreKeyLacksTheToolchainIdentitySegment()
    {
        WorkflowFile workflow;
        QVERIFY2(loadWorkflow(workflow), qPrintable(workflow.parseError));

        const QVector<WorkflowStep> caches =
            stepsUsingAction(workflow.steps, QStringLiteral("actions/cache@"));
        QVERIFY2(caches.size() == 3,
                 qPrintable(QStringLiteral("expected 3 cache steps; observed %1")
                                .arg(caches.size())));

        for (const WorkflowStep &step : caches) {
            QVERIFY2(step.hasRestoreKeys,
                     qPrintable(QStringLiteral("cache step '%1' in job '%2' has no restore-keys "
                                               "block")
                                    .arg(step.name, step.job)));
            QVERIFY2(!step.restoreKeys.isEmpty(),
                     qPrintable(QStringLiteral("cache step '%1' in job '%2' has an empty "
                                               "restore-keys block")
                                    .arg(step.name, step.job)));
            for (const QString &entry : step.restoreKeys) {
                QVERIFY2(entry.contains(kIdentityOutputToken),
                         qPrintable(QStringLiteral(
                                        "cache step '%1' in job '%2': restore-key '%3' lacks "
                                        "the '%4' segment; a broad cross-identity fallback can "
                                        "match binaries built by a different runner image")
                                        .arg(step.name, step.job, entry, kIdentityOutputToken)));
            }
        }
    }

    void workflowDispatchTriggerIsPresent()
    {
        WorkflowFile workflow;
        QVERIFY2(loadWorkflow(workflow), qPrintable(workflow.parseError));

        const QString onBlock = workflow.onBlockLines.join(QLatin1Char('\n'));
        static const QRegularExpression dispatchRe(
            QStringLiteral("^\\s*workflow_dispatch\\s*:"),
            QRegularExpression::MultilineOption);
        QVERIFY2(dispatchRe.match(onBlock).hasMatch(),
                 "the top-level 'on:' trigger block does not include workflow_dispatch; the "
                 "workflow cannot be started manually for warm-cache verification");
    }

    void pushAndPullRequestTriggersRemainPresent()
    {
        WorkflowFile workflow;
        QVERIFY2(loadWorkflow(workflow), qPrintable(workflow.parseError));

        const QString onBlock = workflow.onBlockLines.join(QLatin1Char('\n'));
        static const QRegularExpression pushRe(
            QStringLiteral("^\\s*push\\s*:"), QRegularExpression::MultilineOption);
        static const QRegularExpression prRe(
            QStringLiteral("^\\s*pull_request\\s*:"), QRegularExpression::MultilineOption);
        QVERIFY2(pushRe.match(onBlock).hasMatch(),
                 "the top-level 'on:' trigger block no longer includes the push trigger");
        QVERIFY2(prRe.match(onBlock).hasMatch(),
                 "the top-level 'on:' trigger block no longer includes the pull_request "
                 "trigger");

        const QStringList pushBranches = nestedBlock(workflow.onBlockLines, QStringLiteral("push"));
        QVERIFY2(pushBranches.join(QLatin1Char('\n')).contains(QStringLiteral("main")),
                 "the push trigger no longer includes the 'main' branch");
        QVERIFY2(pushBranches.join(QLatin1Char('\n'))
                     .contains(QStringLiteral("plc-hmi-implementation")),
                 "the push trigger no longer includes the 'plc-hmi-implementation' branch");
    }

    void jobKeysRemainExactlyBuildPackageVisionOff()
    {
        WorkflowFile workflow;
        QVERIFY2(loadWorkflow(workflow), qPrintable(workflow.parseError));

        QStringList observed = workflow.jobKeys;
        observed.sort();
        QStringList expected = { QStringLiteral("build"), QStringLiteral("package"),
                                 QStringLiteral("vision-off") };
        expected.sort();
        QCOMPARE(observed, expected);
    }

    void recordedActionPinsRemainPresent()
    {
        WorkflowFile workflow;
        QVERIFY2(loadWorkflow(workflow), qPrintable(workflow.parseError));

        const QStringList pins = {
            QStringLiteral("actions/checkout@3d3c42e5aac5ba805825da76410c181273ba90b1"),
            QStringLiteral("ilammy/msvc-dev-cmd@0b201ec74fa43914dc39ae48a89fd1d8cb592756"),
            QStringLiteral("lukka/run-vcpkg@b1a0dd252f06b9e25b3c022a9a03bd7a427fb6a2"),
            QStringLiteral("actions/cache@55cc8345863c7cc4c66a329aec7e433d2d1c52a9"),
            QStringLiteral(
                "actions/upload-artifact@043fb46d1a93c77aae656e7c1c64a875d1fc6a0a"),
        };
        for (const QString &pin : pins) {
            QVERIFY2(workflow.text.contains(QStringLiteral("uses: ") + pin),
                     qPrintable(QStringLiteral("recorded action pin '%1' is no longer present "
                                               "as a 'uses:' reference")
                                    .arg(pin)));
        }
    }

    void canonicalCtestAndDeployCheckInvocationsRemainPresent()
    {
        WorkflowFile workflow;
        QVERIFY2(loadWorkflow(workflow), qPrintable(workflow.parseError));

        const qsizetype ctestInvocations = workflow.text.count(QStringLiteral("ctest --test-dir"));
        QVERIFY2(ctestInvocations >= 2,
                 qPrintable(QStringLiteral(
                                "expected at least 2 occurrences of 'ctest --test-dir'; observed "
                                "%1")
                                .arg(ctestInvocations)));

        const qsizetype deployCheckInvocations =
            workflow.text.count(QStringLiteral("check_deploy_deps.ps1"));
        QVERIFY2(deployCheckInvocations >= 2,
                 qPrintable(QStringLiteral(
                                "expected at least 2 occurrences of 'check_deploy_deps.ps1'; "
                                "observed %1")
                                .arg(deployCheckInvocations)));
    }

    void idExpressionChangesWhenOnlyTheRunnerImageVersionDiffers()
    {
        WorkflowFile workflow;
        QVERIFY2(loadWorkflow(workflow), qPrintable(workflow.parseError));

        const QVector<WorkflowStep> captures = captureStepsIn(workflow.steps);
        QVERIFY2(captures.size() == 3,
                 qPrintable(QStringLiteral("expected 3 capture steps; observed %1")
                                .arg(captures.size())));

        for (const WorkflowStep &step : captures) {
            const QString token = imageTokenInEmission(step);
            QVERIFY2(!token.isEmpty(),
                     qPrintable(QStringLiteral(
                                    "capture step '%1' in job '%2': the emitted id does not "
                                    "reference the runner image version ($env:ImageVersion), so "
                                    "an image rollover with an unchanged compiler version string "
                                    "would produce the same cache identity")
                                    .arg(step.name, step.job)));

            QString imageA = emissionLines(step).join(QLatin1Char('\n'));
            QString imageB = imageA;
            imageA.replace(token, QStringLiteral("HLM_IMAGE_A"));
            imageB.replace(token, QStringLiteral("HLM_IMAGE_B"));
            QVERIFY2(imageA != imageB,
                     qPrintable(QStringLiteral(
                                    "capture step '%1' in job '%2': substituting only the runner "
                                    "image version does not change the emitted id expression")
                                    .arg(step.name, step.job)));
        }
    }

    void noRestoreKeyCanMatchADifferentToolchainIdentity()
    {
        WorkflowFile workflow;
        QVERIFY2(loadWorkflow(workflow), qPrintable(workflow.parseError));

        const QVector<WorkflowStep> caches =
            stepsUsingAction(workflow.steps, QStringLiteral("actions/cache@"));
        QVERIFY2(caches.size() == 3,
                 qPrintable(QStringLiteral("expected 3 cache steps; observed %1")
                                .arg(caches.size())));

        for (const WorkflowStep &step : caches) {
            QVERIFY2(!step.restoreKeys.isEmpty(),
                     qPrintable(QStringLiteral("cache step '%1' in job '%2' has no restore-keys "
                                               "entries")
                                    .arg(step.name, step.job)));
            for (const QString &entry : step.restoreKeys) {
                QVERIFY2(entry != kLegacyBroadRestoreKey,
                         qPrintable(QStringLiteral(
                                        "cache step '%1' in job '%2' still contains the broad "
                                        "cross-identity restore-key '%3'")
                                        .arg(step.name, step.job, kLegacyBroadRestoreKey)));
                QVERIFY2(entry.contains(kIdentityOutputToken),
                         qPrintable(QStringLiteral(
                                        "cache step '%1' in job '%2': restore-key '%3' can match "
                                        "a different toolchain identity (no '%4' segment)")
                                        .arg(step.name, step.job, entry, kIdentityOutputToken)));
            }
        }
    }

    void theThreeCacheStepsShareIdenticalKeyAndRestoreKeyExpressions()
    {
        WorkflowFile workflow;
        QVERIFY2(loadWorkflow(workflow), qPrintable(workflow.parseError));

        const QVector<WorkflowStep> caches =
            stepsUsingAction(workflow.steps, QStringLiteral("actions/cache@"));
        QVERIFY2(caches.size() == 3,
                 qPrintable(QStringLiteral("expected 3 cache steps; observed %1")
                                .arg(caches.size())));

        for (qsizetype i = 1; i < caches.size(); ++i) {
            QVERIFY2(caches.at(i).key == caches.at(0).key,
                     qPrintable(QStringLiteral(
                                    "cache step '%1' diverges from cache step '%2': key '%3' vs "
                                    "'%4'")
                                    .arg(caches.at(i).name, caches.at(0).name, caches.at(i).key,
                                         caches.at(0).key)));
            QVERIFY2(caches.at(i).restoreKeys == caches.at(0).restoreKeys,
                     qPrintable(QStringLiteral(
                                    "cache step '%1' diverges from cache step '%2': restore-keys "
                                    "[%3] vs [%4]")
                                    .arg(caches.at(i).name, caches.at(0).name,
                                         caches.at(i).restoreKeys.join(QStringLiteral(" | ")),
                                         caches.at(0).restoreKeys.join(QStringLiteral(" | ")))));
        }
    }

    void buildMatrixConfigurationsShareOneCaptureAndCacheExpression()
    {
        WorkflowFile workflow;
        QVERIFY2(loadWorkflow(workflow), qPrintable(workflow.parseError));

        // Both matrix configurations (Debug, Release) iterate the same step
        // definitions, so the shared-expression property is: the build job
        // defines exactly one capture step and exactly one cache step, and
        // neither is made configuration-specific by an if: guard.
        int captureSteps = 0;
        int cacheSteps = 0;
        for (const WorkflowStep &step : workflow.steps) {
            if (step.job != QLatin1String("build"))
                continue;
            if (step.run.contains(kImageVariable)) {
                ++captureSteps;
                QVERIFY2(!step.ifCondition.contains(QStringLiteral("matrix")),
                         qPrintable(QStringLiteral(
                                        "capture step '%1' in the build matrix job is guarded by "
                                        "if: %2, so Debug and Release would capture different "
                                        "toolchain identities")
                                        .arg(step.name, step.ifCondition)));
            }
            if (step.uses.startsWith(QStringLiteral("actions/cache@"))) {
                ++cacheSteps;
                QVERIFY2(!step.ifCondition.contains(QStringLiteral("matrix")),
                         qPrintable(QStringLiteral(
                                        "cache step '%1' in the build matrix job is guarded by "
                                        "if: %2, so Debug and Release would use different cache "
                                        "expressions")
                                        .arg(step.name, step.ifCondition)));
            }
        }
        QVERIFY2(captureSteps == 1,
                 qPrintable(QStringLiteral(
                                "expected exactly 1 capture step in the build matrix job so both "
                                "Debug and Release share it; observed %1")
                                .arg(captureSteps)));
        QVERIFY2(cacheSteps == 1,
                 qPrintable(QStringLiteral(
                                "expected exactly 1 actions/cache step in the build matrix job so "
                                "both Debug and Release share it; observed %1")
                                .arg(cacheSteps)));
    }

    void captureScriptKeepsFallbacksForMissingImageVersionAndCompilerBinary()
    {
        WorkflowFile workflow;
        QVERIFY2(loadWorkflow(workflow), qPrintable(workflow.parseError));

        const QVector<WorkflowStep> captures = captureStepsIn(workflow.steps);
        QVERIFY2(captures.size() == 3,
                 qPrintable(QStringLiteral("expected 3 capture steps; observed %1")
                                .arg(captures.size())));

        // Boundary: an empty ImageVersion must still leave a usable id, so the
        // value that reaches the id emission must be fallback-guarded.
        // Boundary: the compiler-binary hash is best effort and must not fail
        // the workflow when cl.exe is absent on the runner image.
        static const QRegularExpression compilerGuardRe(
            QStringLiteral("Test-Path|-ErrorAction|-EA\\b|\\btry\\b|\\bcatch\\b|"
                           "\\?\\?|\\bif\\b|\\belse\\b"));

        for (const WorkflowStep &step : captures) {
            QVERIFY2(imageValueInIdEmissionIsFallbackGuarded(step),
                     qPrintable(QStringLiteral(
                                    "capture step '%1' in job '%2' has no fallback for an empty "
                                    "$env:ImageVersion (or emits it without a guard); the emitted "
                                    "id must stay usable")
                                    .arg(step.name, step.job)));
            QVERIFY2(containsGuard(step, compilerGuardRe),
                     qPrintable(QStringLiteral(
                                    "capture step '%1' in job '%2' has no guard for an absent "
                                    "compiler binary; the command hash is best effort and must "
                                    "not fail the workflow")
                                     .arg(step.name, step.job)));
        }
    }

    void theThreeCaptureStepsShareTextuallyIdenticalIdentityScripts()
    {
        WorkflowFile workflow;
        QVERIFY2(loadWorkflow(workflow), qPrintable(workflow.parseError));

        const QVector<WorkflowStep> captures = captureStepsIn(workflow.steps);
        QVERIFY2(captures.size() == 3,
                 qPrintable(QStringLiteral("expected 3 capture steps; observed %1")
                                .arg(captures.size())));

        // Boundary (brief): the three jobs must not diverge in identity
        // semantics; the capture scripts must be textually identical.
        const QString reference = captures.at(0).run;
        for (qsizetype i = 1; i < captures.size(); ++i) {
            QVERIFY2(captures.at(i).run == reference,
                     qPrintable(QStringLiteral(
                                    "capture step in job '%1' diverges from the capture step in "
                                    "job '%2': the identity script must stay textually identical "
                                    "so every job derives the same id semantics")
                                    .arg(captures.at(i).job, captures.at(0).job)));
        }
        QVERIFY2(reference.contains(kImageVariable),
                 "the shared capture script no longer references $env:ImageVersion");
        QVERIFY2(hashesCompilerBinary(captures.at(0)),
                 "the shared capture script no longer hashes the compiler binary at "
                 "$env:VCToolsInstallDir\\bin\\HostX64\\x64\\cl.exe");
    }

    void theThreeCacheStepsKeepExactlyOneIdentityRestoreKeyEach()
    {
        WorkflowFile workflow;
        QVERIFY2(loadWorkflow(workflow), qPrintable(workflow.parseError));

        const QVector<WorkflowStep> caches =
            stepsUsingAction(workflow.steps, QStringLiteral("actions/cache@"));
        QVERIFY2(caches.size() == 3,
                 qPrintable(QStringLiteral("expected 3 cache steps; observed %1")
                                .arg(caches.size())));

        // The no-broad-fallback property holds only if each cache block keeps
        // exactly one restore key and that single key carries the toolchain
        // identity; a second, broader entry would reintroduce the fallback.
        for (const WorkflowStep &step : caches) {
            QVERIFY2(step.hasRestoreKeys,
                     qPrintable(QStringLiteral("cache step '%1' in job '%2' has no restore-keys "
                                               "block")
                                    .arg(step.name, step.job)));
            QVERIFY2(step.restoreKeys.size() == 1,
                     qPrintable(QStringLiteral(
                                    "cache step '%1' in job '%2' declares %3 restore-keys "
                                    "entries; the block must keep exactly one same-identity "
                                    "restore key so no broader entry can match a different "
                                    "toolchain identity: [%4]")
                                    .arg(step.name, step.job)
                                    .arg(step.restoreKeys.size())
                                    .arg(step.restoreKeys.join(QStringLiteral(" | ")))));
            QVERIFY2(step.restoreKeys.at(0).contains(kIdentityOutputToken),
                     qPrintable(QStringLiteral(
                                    "cache step '%1' in job '%2': the single restore key '%3' "
                                    "lacks the '%4' segment")
                                    .arg(step.name, step.job, step.restoreKeys.at(0),
                                         kIdentityOutputToken)));
        }
    }
};

QTEST_GUILESS_MAIN(CiWorkflowStructureTest)
#include "ci_workflow_structure_test.moc"
