#pragma once

#include <QWidget>
#include <QLabel>

namespace hlm {

class ShellModel;

// Top alarm banner, ~40 px (spec §11.1): green "无报警" when no alarm;
// highest-priority active alarm text otherwise (estop > fault > offline).
class AlarmBanner : public QWidget
{
    Q_OBJECT

public:
    explicit AlarmBanner(ShellModel &model, QWidget *parent = nullptr);

    QString text() const { return m_label->text(); }

public slots:
    void refresh();

private:
    ShellModel &m_model;
    QLabel *m_label = nullptr;
};

} // namespace hlm