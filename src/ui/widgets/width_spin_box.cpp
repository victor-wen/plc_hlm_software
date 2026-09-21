#include "ui/widgets/width_spin_box.h"

#include "domain/width_units.h"

#include <QRegularExpression>
#include <QVariant>

namespace hlm {

WidthSpinBox::WidthSpinBox(QWidget *parent)
    : QSpinBox(parent)
{
    // Raw 0.1 mm units; the operator-facing range is "> 0" with no upper
    // bound other than the u16 register itself (user decision 2026-09-21).
    setRange(1, 65535);
    // One step is one raw unit == 0.1 mm.
    setSingleStep(1);
    setSuffix(QStringLiteral(" mm"));
    setValue(width_units::kNeutralTargetRaw);
    setAlignment(Qt::AlignRight | Qt::AlignVCenter);
}

QString WidthSpinBox::textFromValue(int value) const
{
    // QSpinBox appends the suffix itself; only the number is scaled here.
    return QString::number(value / double(width_units::kRawPerMm), 'f', 1);
}

int WidthSpinBox::valueFromText(const QString &text) const
{
    // Accepts the decimal form the operator sees ("200.5 mm", "200.5", "200")
    // and returns raw 0.1 mm units. Anything unparseable falls back to the
    // current value so a transient editing state cannot write a stray value.
    QString cleaned = text;
    cleaned.remove(suffix());
    cleaned.remove(prefix());
    static const QRegularExpression numeric(QStringLiteral("[0-9]+(\\.[0-9]*)?"));
    const QRegularExpressionMatch match = numeric.match(cleaned);
    if (!match.hasMatch())
        return value();
    bool ok = false;
    const double mm = match.captured(0).toDouble(&ok);
    if (!ok)
        return value();
    return width_units::mmToRaw(mm);
}

QValidator::State WidthSpinBox::validate(QString &text, int &pos) const
{
    Q_UNUSED(pos);
    QString cleaned = text;
    cleaned.remove(suffix());
    cleaned.remove(prefix());
    cleaned = cleaned.trimmed();
    if (cleaned.isEmpty())
        return QValidator::Intermediate; // still typing

    // A complete decimal: range-checked so out-of-range entry is refused the
    // way an integer spin box refuses it.
    static const QRegularExpression complete(QStringLiteral("^[0-9]+(\\.[0-9]*)?$"));
    if (complete.match(cleaned).hasMatch()) {
        bool ok = false;
        const double mm = cleaned.toDouble(&ok);
        if (!ok)
            return QValidator::Invalid;
        const long raw = std::lround(mm * double(width_units::kRawPerMm));
        return (raw >= minimum() && raw <= maximum()) ? QValidator::Acceptable
                                                      : QValidator::Invalid;
    }
    // A prefix of a decimal ("", "20", "20.", "20.5") is still in progress.
    static const QRegularExpression partial(QStringLiteral("^[0-9]*\\.?[0-9]*$"));
    if (partial.match(cleaned).hasMatch())
        return QValidator::Intermediate;
    return QValidator::Invalid;
}

} // namespace hlm