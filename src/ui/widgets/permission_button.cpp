#include "ui/widgets/permission_button.h"

#include <QFont>
#include <QLabel>
#include <QResizeEvent>
#include <QVBoxLayout>

namespace hlm {

namespace {

// Touch-visible reason text must stay legible at touch sizes: the frozen floor
// is a font metrics height of at least 12 px (PLC-HMI-008 OB-5). The reason
// uses its own compact font so long permission/interlock texts stay readable
// inside a 192 px action bar; the guard enforces the legibility floor even if
// a compact platform font metrics would fall below it.
//
// The button-level stylesheets (theme QSS for the action bar, the estop danger
// sheet) set a 17 px font that Qt's stylesheet style otherwise cascades into
// the child label. A label-level rule keeps the compact size authoritative;
// the reserved height is capped at kReasonMaximumLines so a pathological text
// can never inflate the surrounding layout.
constexpr int kReasonPixelSize = 12;
constexpr int kReasonMinimumFontHeight = 12;
constexpr int kReasonHorizontalMargin = 6;
constexpr int kReasonMaximumLines = 3;

void applyReasonFont(QLabel *label)
{
    QFont font = label->font();
    font.setPixelSize(kReasonPixelSize);
    label->setFont(font);
    label->setStyleSheet(QStringLiteral("font-size: %1px;")
                             .arg(kReasonPixelSize));
    if (label->fontMetrics().height() >= kReasonMinimumFontHeight)
        return;
    font.setPixelSize(kReasonPixelSize + 2);
    label->setFont(font);
    label->setStyleSheet(QStringLiteral("font-size: %1px;")
                             .arg(kReasonPixelSize + 2));
}

int reasonHeightCap(const QLabel *label)
{
    return kReasonMaximumLines * label->fontMetrics().lineSpacing();
}

} // namespace

PermissionButton::PermissionButton(QWidget *parent)
    : PermissionButton(QString(), parent)
{
}

PermissionButton::PermissionButton(const QString &text, QWidget *parent)
    : QPushButton(text, parent)
{
    setMinimumSize(48, 48);
    // The title stays painted at the top of the control; the reason region is
    // laid out at the bottom (D3). Qt's stylesheet text-align is the only
    // supported way to move QPushButton's painted text; only that property is
    // set here so the theme keeps full control of every colour/background.
    // ActionBar re-applies its danger sheet to the estop button and must keep
    // the same text-align (see action_bar.cpp).
    setStyleSheet(QStringLiteral("text-align: top;"));
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(2, 2, 2, 2);
    layout->setSpacing(0);
    layout->addStretch(1);
    m_reasonLabel = new QLabel(this);
    m_reasonLabel->setObjectName(QStringLiteral("permissionReason"));
    m_reasonLabel->setAlignment(Qt::AlignHCenter | Qt::AlignBottom);
    m_reasonLabel->setWordWrap(true);
    m_reasonLabel->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    applyReasonFont(m_reasonLabel);
    m_reasonLabel->setMaximumHeight(reasonHeightCap(m_reasonLabel));
    m_reasonLabel->hide();
    layout->addWidget(m_reasonLabel);
}

QString PermissionButton::visibleReasonText() const
{
    return m_reasonLabel != nullptr ? m_reasonLabel->text().trimmed() : QString();
}

void PermissionButton::setEnabledWithReason(bool allowed, const QString &reason)
{
    m_allowed = allowed;
    m_reason = allowed ? QString() : reason;
    apply();
}

void PermissionButton::showEvent(QShowEvent *e)
{
    QPushButton::showEvent(e);
    apply();
}

void PermissionButton::resizeEvent(QResizeEvent *e)
{
    QPushButton::resizeEvent(e);
    clampReasonHeight();
}

void PermissionButton::clampReasonHeight()
{
    if (m_reasonLabel == nullptr)
        return;
    const int fullCap = reasonHeightCap(m_reasonLabel);
    // While the reason is shown, never let it grow into the top-painted title:
    // a tight layout clamps the visible reason region below the title instead
    // of drawing over it. With enough room the whole reason stays visible.
    const int maximum =
        m_reasonLabel->isHidden()
            ? fullCap
            : qBound(0, height() - (fontMetrics().height() + 6), fullCap);
    if (m_reasonLabel->maximumHeight() != maximum)
        m_reasonLabel->setMaximumHeight(maximum);
}

void PermissionButton::apply()
{
    // m_allowed is the permission/interlock verdict. The widget is enabled
    // exactly when allowed; the reason is shown as inline visible text, with
    // the tooltip/status tip kept as supplements (spec §11.4, D3). External
    // disables (e.g. a disabled parent) still win because Qt combines them.
    setToolTip(m_reason);
    setStatusTip(m_reason);
    if (m_reasonLabel != nullptr) {
        m_reasonLabel->setText(m_reason);
        m_reasonLabel->setVisible(!m_reason.isEmpty());
        clampReasonHeight();
    }
    setEnabled(m_allowed);
    // The reserved reason height depends on the current reason text.
    updateGeometry();
}

QSize PermissionButton::withReasonHeight(const QSize &base) const
{
    if (m_reasonLabel == nullptr || m_reasonLabel->isHidden())
        return base;
    const int available = width() - 2 * kReasonHorizontalMargin;
    const int reasonHeight = available > 0
                                 ? m_reasonLabel->heightForWidth(available)
                                 : m_reasonLabel->sizeHint().height();
    QSize hint = base;
    // The cap keeps the reserved region bounded even for pathological texts;
    // the label itself is clamped to the same height.
    hint.setHeight(hint.height() + qBound(0, reasonHeight, reasonHeightCap(m_reasonLabel)));
    return hint;
}

QSize PermissionButton::sizeHint() const
{
    QSize base = QPushButton::sizeHint();
    base.setHeight(qMax(base.height(), minimumHeight()));
    return withReasonHeight(base);
}

QSize PermissionButton::minimumSizeHint() const
{
    QSize base = QPushButton::minimumSizeHint();
    base.setHeight(qMax(base.height(), minimumHeight()));
    return withReasonHeight(base);
}

} // namespace hlm
