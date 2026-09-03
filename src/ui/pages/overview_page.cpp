#include "ui/pages/overview_page.h"

#include "ui/shell/shell_model.h"
#include "ui/widgets/value_display.h"

#include <QLabel>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QFrame>
#include <QHash>

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
    auto *schematicLabel = new QLabel(QStringLiteral("设备示意"), schematic);
    schematicLabel->setAlignment(Qt::AlignCenter);
    schematicLayout->addWidget(schematicLabel);
    root->addWidget(schematic, /*stretch=*/1);

    // --- value grid -------------------------------------------------------------
    auto *grid = new QGridLayout();
    grid->setHorizontalSpacing(32);
    grid->setVerticalSpacing(12);
    auto *leftColumn = new QVBoxLayout();
    auto *rightColumn = new QVBoxLayout();
    leftColumn->addWidget(addField(QStringLiteral("step"),
                                   QStringLiteral("当前步骤 (D120)"), nullptr));
    leftColumn->addWidget(addField(QStringLiteral("targetWidth"),
                                   QStringLiteral("目标宽度 (D128)"), nullptr));
    leftColumn->addWidget(addField(QStringLiteral("currentWidth"),
                                   QStringLiteral("当前宽度 (D130)"), nullptr));
    rightColumn->addWidget(addField(QStringLiteral("widthDelta"),
                                    QStringLiteral("调宽差值 (D210)"), nullptr));
    rightColumn->addWidget(addField(QStringLiteral("beltSpeed"),
                                    QStringLiteral("皮带速度 (D122)"), nullptr));
    rightColumn->addWidget(addField(QStringLiteral("productionCount"),
                                    QStringLiteral("累计产量 (D138)"), nullptr));
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
}

ValueDisplay *OverviewPage::addField(const QString &key, const QString &title,
                                     QVBoxLayout *column)
{
    Q_UNUSED(column);
    auto *titleWrap = new QWidget(this);
    auto *layout = new QVBoxLayout(titleWrap);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);
    auto *titleLabel = new QLabel(title, titleWrap);
    layout->addWidget(titleLabel);
    auto *display = new ValueDisplay(titleWrap);
    display->setMinimumHeight(48);
    layout->addWidget(display);
    m_displays.insert(key, display);
    return display;
}

ValueDisplay *OverviewPage::fieldDisplay(const QString &key) const
{
    return m_displays.value(key, nullptr);
}

QLabel *OverviewPage::latestAlarmLabel() const
{
    return m_alarmLabel;
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

    // Latest alarm line.
    m_alarmLabel->setText(m_pageModel.latestAlarmText());
}

} // namespace hlm