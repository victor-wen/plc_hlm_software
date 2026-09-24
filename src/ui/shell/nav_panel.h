#pragma once

#include <QWidget>
#include <QVector>
#include <QString>

class QListWidget;
class QListWidgetItem;

namespace hlm {

// Left navigation, ~176 px, 7 items (spec §11.1; I/O 与诊断 removed
// 2026-09-24, 扫码服务 moved ahead of 用户与设置):
// 总览、配方与调宽、手动控制、报警、操作记录、扫码服务、用户与设置.
class NavPanel : public QWidget
{
    Q_OBJECT

public:
    explicit NavPanel(QWidget *parent = nullptr);

    int itemCount() const;
    int currentItem() const;
    int itemMinimumHeight() const;
    QString pageTitle(int index) const;

public slots:
    void setCurrent(int index);

signals:
    void pageSelected(int index);

private:
    QListWidget *m_list = nullptr;
};

} // namespace hlm
