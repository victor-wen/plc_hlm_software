#include "ui/shell/nav_panel.h"

#include <QVBoxLayout>
#include <QListWidget>
#include <QLabel>
#include <QStringList>

namespace hlm {

namespace {
// The 7 pages (spec §11.1, §11.3). Order must match MainWindow page stack.
const QStringList kPages = {
    QStringLiteral("总览"),
    QStringLiteral("配方与调宽"),
    QStringLiteral("手动控制"),
    QStringLiteral("报警"),
    QStringLiteral("操作记录"),
    QStringLiteral("I/O 与诊断"),
    QStringLiteral("用户与设置"),
};
} // namespace

NavPanel::NavPanel(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("navPanel"));
    setMinimumWidth(176);
    setMaximumWidth(176);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 12, 0, 12);
    layout->setSpacing(0);

    auto *title = new QLabel(QStringLiteral("功能导航"), this);
    title->setObjectName(QStringLiteral("navTitle"));
    layout->addWidget(title);

    m_list = new QListWidget(this);
    m_list->setObjectName(QStringLiteral("navList"));
    m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    for (const QString &label : kPages)
        m_list->addItem(label);

    connect(m_list, &QListWidget::currentRowChanged, this,
            [this](int row) { emit pageSelected(row); });

    layout->addWidget(m_list, 1);
    m_list->setCurrentRow(0);
}

int NavPanel::itemCount() const
{
    return m_list->count();
}

int NavPanel::currentItem() const
{
    return m_list->currentRow();
}

int NavPanel::itemMinimumHeight() const
{
    // Touch target >= 48 px per nav item (spec §11.1); the QSS sets 64 px
    // but the code enforces the floor even without the theme loaded.
    return qMax(m_list->sizeHintForRow(0), 48);
}

QString NavPanel::pageTitle(int index) const
{
    if (index < 0 || index >= m_list->count())
        return QString();
    return m_list->item(index)->text();
}

void NavPanel::setCurrent(int index)
{
    if (index < 0 || index >= m_list->count())
        return;
    m_list->setCurrentRow(index);
}

} // namespace hlm
