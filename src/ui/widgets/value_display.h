#pragma once

#include <QWidget>
#include <QString>

namespace hlm {

// Shared industrial value display (spec §9, §11.2). Shows "value + unit"
// when the field is valid and fresh; shows "—" when the value is invalid
// or the snapshot is stale. The caller decides validity from the snapshot
// (fieldValid + quality); this widget only renders.
class ValueDisplay : public QWidget
{
    Q_OBJECT

public:
    explicit ValueDisplay(QWidget *parent = nullptr);

    // `valid` false -> shows "—" and greys the value (spec §11.2).
    void setValue(const QString &value, const QString &unit, bool valid);
    QString text() const;
    bool isValid() const { return m_valid; }

protected:
    void paintEvent(QPaintEvent *event) override;
    QSize minimumSizeHint() const override;

private:
    QString m_value;
    QString m_unit;
    bool m_valid = false;
};

} // namespace hlm