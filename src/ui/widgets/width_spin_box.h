#pragma once

#include <QSpinBox>

namespace hlm {

// Operator width editor for D128 (user decision 2026-09-21: the register unit
// is 0.1 mm). QSpinBox is integer-only and has no setDecimals(), so the
// scaling lives in the text conversion instead of in the value:
//
//   value()      -> RAW register units (2000 == 200.0 mm), never millimetres
//   displayed    -> "200.0", one step per 0.1 mm (singleStep() == 1)
//
// Keeping the value raw means every consumer downstream (InterlockRules, the
// coordinator's D128 write, the recipe record) stays in the same domain as the
// PLC, so no consumer can be off by a factor of ten. Only valueFromText() and
// textFromValue() know about the decimal point.
class WidthSpinBox : public QSpinBox
{
    Q_OBJECT

public:
    explicit WidthSpinBox(QWidget *parent = nullptr);

protected:
    QString textFromValue(int value) const override;
    int valueFromText(const QString &text) const override;
    // QSpinBox's built-in validator is integer-only, so the decimal form the
    // operator sees would be rejected before valueFromText() is consulted.
    QValidator::State validate(QString &text, int &pos) const override;
};

} // namespace hlm
