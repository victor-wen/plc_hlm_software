#include "ui/pages/recipe_width_page.h"

#include "ui/shell/shell_model.h"
#include "ui/widgets/value_display.h"
#include "ui/widgets/permission_button.h"

#include <QLabel>
#include <QLineEdit>
#include <QSpinBox>
#include <QListWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QFrame>
#include <QHash>

namespace hlm {

RecipeWidthPage::RecipeWidthPage(ShellModel &model, QWidget *parent)
    : QWidget(parent)
    , m_model(model)
    , m_pageModel(model, this)
{
    setObjectName(QStringLiteral("recipeWidthPage"));
    buildLayout();
    connect(&m_model, &ShellModel::stateChanged, this, &RecipeWidthPage::refresh);
    refresh();
}

void RecipeWidthPage::buildLayout()
{
    // Qt Layout only, no absolute coordinates (spec §11.1). Structure follows
    // 需求/PLC上位机地址及要求.txt 第七节: 宽度控制区 (目标宽度输入、当前宽度
    // 显示、调宽速度) + 配方管理 (保存多个宽度值, 选择后载入 D128 目标).
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(16, 16, 16, 16);
    root->setSpacing(12);

    // --- width control area (宽度控制区) --------------------------------------
    auto *widthBox = new QFrame(this);
    widthBox->setObjectName(QStringLiteral("recipeWidthPanel"));
    widthBox->setFrameShape(QFrame::StyledPanel);
    auto *widthLayout = new QVBoxLayout(widthBox);
    widthLayout->setSpacing(8);
    auto *widthTitle = new QLabel(QStringLiteral("宽度控制"), widthBox);
    widthLayout->addWidget(widthTitle);

    auto *grid = new QGridLayout();
    grid->setHorizontalSpacing(24);
    grid->setVerticalSpacing(8);
    auto *leftColumn = new QVBoxLayout();
    auto *rightColumn = new QVBoxLayout();
    leftColumn->addWidget(addField(QStringLiteral("targetWidth"),
                                   QStringLiteral("目标宽度 (D128)"), nullptr));
    leftColumn->addWidget(addField(QStringLiteral("currentWidth"),
                                   QStringLiteral("当前宽度 (D130)"), nullptr));
    leftColumn->addWidget(addField(QStringLiteral("widthDelta"),
                                   QStringLiteral("调宽差值 (D210)"), nullptr));
    rightColumn->addWidget(addField(QStringLiteral("pulsePerMm"),
                                    QStringLiteral("脉冲当量 (D204)"), nullptr));
    rightColumn->addWidget(addField(QStringLiteral("widthSpeed"),
                                    QStringLiteral("调宽速度 (D220)"), nullptr));
    grid->addLayout(leftColumn, 0, 0);
    grid->addLayout(rightColumn, 0, 1);
    grid->setColumnStretch(0, 1);
    grid->setColumnStretch(1, 1);
    widthLayout->addLayout(grid);

    // --- recipe management (配方管理) ------------------------------------------
    auto *recipeBox = new QFrame(this);
    recipeBox->setObjectName(QStringLiteral("recipePanel"));
    recipeBox->setFrameShape(QFrame::StyledPanel);
    auto *recipeLayout = new QVBoxLayout(recipeBox);
    recipeLayout->setSpacing(8);
    auto *recipeTitle = new QLabel(QStringLiteral("配方管理"), recipeBox);
    recipeLayout->addWidget(recipeTitle);

    m_recipeList = new QListWidget(recipeBox);
    m_recipeList->setObjectName(QStringLiteral("recipeList"));
    m_recipeList->setMinimumHeight(120);
    recipeLayout->addWidget(m_recipeList);

    auto *editRow = new QHBoxLayout();
    editRow->setSpacing(8);
    auto *nameLabel = new QLabel(QStringLiteral("名称"), recipeBox);
    editRow->addWidget(nameLabel);
    m_nameEdit = new QLineEdit(recipeBox);
    m_nameEdit->setObjectName(QStringLiteral("recipeNameEdit"));
    m_nameEdit->setPlaceholderText(QStringLiteral("配方名称"));
    editRow->addWidget(m_nameEdit, /*stretch=*/1);
    auto *widthLabel = new QLabel(QStringLiteral("宽度 mm"), recipeBox);
    editRow->addWidget(widthLabel);
    m_widthSpin = new QSpinBox(recipeBox);
    m_widthSpin->setObjectName(QStringLiteral("recipeWidthSpin"));
    m_widthSpin->setRange(50, 400); // D128 range (spec §10.3)
    m_widthSpin->setValue(50);
    editRow->addWidget(m_widthSpin);
    recipeLayout->addLayout(editRow);

    auto *buttonRow = new QHBoxLayout();
    buttonRow->setSpacing(8);
    m_save = new PermissionButton(QStringLiteral("保存配方"), recipeBox);
    m_save->setObjectName(QStringLiteral("recipeSaveButton"));
    m_save->setMinimumHeight(48);
    buttonRow->addWidget(m_save);
    m_delete = new PermissionButton(QStringLiteral("删除配方"), recipeBox);
    m_delete->setObjectName(QStringLiteral("recipeDeleteButton"));
    m_delete->setMinimumHeight(48);
    buttonRow->addWidget(m_delete);
    buttonRow->addStretch();
    recipeLayout->addLayout(buttonRow);

    // --- apply row + status -----------------------------------------------------
    auto *applyRow = new QHBoxLayout();
    applyRow->setSpacing(12);
    m_apply = new PermissionButton(QStringLiteral("应用并调宽"), this);
    m_apply->setObjectName(QStringLiteral("applyWidthButton"));
    m_apply->setMinimumHeight(64);
    applyRow->addWidget(m_apply);
    m_statusLabel = new QLabel(this);
    m_statusLabel->setObjectName(QStringLiteral("adjustStatus"));
    m_statusLabel->setMinimumHeight(48);
    applyRow->addWidget(m_statusLabel, /*stretch=*/1);
    root->addLayout(applyRow);

    root->addWidget(widthBox, /*stretch=*/1);
    root->addWidget(recipeBox, /*stretch=*/1);

    // --- wiring -----------------------------------------------------------------
    connect(m_apply, &QPushButton::clicked, this, &RecipeWidthPage::onApplyClicked);
    connect(m_save, &QPushButton::clicked, this, &RecipeWidthPage::onSaveClicked);
    connect(m_delete, &QPushButton::clicked, this, &RecipeWidthPage::onDeleteClicked);
    connect(m_recipeList, &QListWidget::currentRowChanged, this,
            &RecipeWidthPage::onRecipeSelected);
    connect(m_nameEdit, &QLineEdit::textChanged, this,
            [this](const QString &t) { m_pageModel.setEditedName(t); });
    connect(m_widthSpin, &QSpinBox::valueChanged, this,
            [this](int v) { m_pageModel.setEditedWidth(v); });
}

ValueDisplay *RecipeWidthPage::addField(const QString &key, const QString &title,
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

ValueDisplay *RecipeWidthPage::fieldDisplay(const QString &key) const
{
    return m_displays.value(key, nullptr);
}

QLabel *RecipeWidthPage::statusLabel() const
{
    return m_statusLabel;
}

QString RecipeWidthPage::statusText() const
{
    return m_statusLabel ? m_statusLabel->text() : QString();
}

void RecipeWidthPage::setRecipes(const QVector<RecipeRecord> &recipes)
{
    m_pageModel.setRecipes(recipes);
    m_recipeList->clear();
    for (const RecipeRecord &r : recipes)
        m_recipeList->addItem(QStringLiteral("%1  (%2 mm)").arg(r.name).arg(r.targetWidthMm));
    refresh();
}

void RecipeWidthPage::setAdjustResult(bool ok, const QString &detail)
{
    m_pageModel.setAdjustResult(ok, detail);
    refresh();
}

void RecipeWidthPage::onApplyClicked()
{
    // Two-step confirmation (spec §10.3: 点击"应用并调宽"并确认后执行).
    if (!m_applyArmed) {
        m_applyArmed = true;
        m_apply->setText(QStringLiteral("确认应用?"));
        return;
    }
    m_applyArmed = false;
    m_apply->setText(QStringLiteral("应用并调宽"));

    // 目标 == 当前: 显示"当前已是目标宽度"并结束, 不发命令 (spec §10.3 step 3).
    if (m_pageModel.targetEqualsCurrent()) {
        refresh();
        return;
    }
    emit applyAdjustRequested(quint16(m_widthSpin->value()));
    m_pageModel.beginApply(quint16(m_widthSpin->value()));
    refresh();
}

void RecipeWidthPage::onRecipeSelected(int row)
{
    if (row < 0 || row >= m_pageModel.recipes().size())
        return;
    // 选择配方只把名称和目标宽度加载到界面, 不写 PLC (spec §10.3).
    m_pageModel.selectRecipe(m_pageModel.recipes().at(row));
    m_nameEdit->setText(m_pageModel.editedName());
    m_widthSpin->setValue(m_pageModel.editedWidth());
    refresh();
}

void RecipeWidthPage::onSaveClicked()
{
    emit saveRecipeRequested(m_nameEdit->text(), m_widthSpin->value());
}

void RecipeWidthPage::onDeleteClicked()
{
    const int row = m_recipeList->currentRow();
    if (row < 0 || row >= m_pageModel.recipes().size())
        return;
    emit deleteRecipeRequested(m_pageModel.recipes().at(row).id);
}

void RecipeWidthPage::refresh()
{
    // Full re-render from the model (spec §9: 完整快照更新, 无乐观更新).

    // Width displays: invalid -> "—" (spec §9).
    const DeviceSnapshot &s = m_model.snapshot();
    const bool fresh = m_model.snapshotFresh();
    const auto field = [&](const QString &key, const QString &text,
                           SnapshotField f, const QString &unit) {
        const bool valid = fresh && s.fieldValid(f);
        m_displays[key]->setValue(valid ? text : QString(), unit, valid);
    };
    field(QStringLiteral("targetWidth"), QString::number(s.targetWidth()),
          SnapshotField::TargetWidth, QStringLiteral("mm"));
    field(QStringLiteral("currentWidth"), QString::number(s.currentWidth()),
          SnapshotField::CurrentWidth, QStringLiteral("mm"));
    field(QStringLiteral("widthDelta"), QString::number(s.widthDelta()),
          SnapshotField::CurrentWidth, QStringLiteral("mm"));
    field(QStringLiteral("pulsePerMm"), QString::number(s.pulsePerMm()),
          SnapshotField::PulsePerMm, QStringLiteral("脉冲/mm"));
    field(QStringLiteral("widthSpeed"), QString::number(s.widthSpeed()),
          SnapshotField::WidthSpeed, QStringLiteral("mm/s"));

    // Apply gating: permission + interlock reasons (spec §11.4).
    const QStringList reasons = m_pageModel.applyUnmetReasons();
    m_apply->setEnabledWithReason(m_pageModel.canApply(),
                                  reasons.join(QStringLiteral("；")));
    const bool canEdit = m_pageModel.canEditRecipes();
    const QString permReason = canEdit ? QString()
                                       : QStringLiteral("需要管理员权限");
    m_save->setEnabledWithReason(canEdit, permReason);
    m_delete->setEnabledWithReason(canEdit, permReason);
    m_nameEdit->setEnabled(canEdit);
    m_widthSpin->setEnabled(canEdit);

    // Status line.
    m_statusLabel->setText(m_pageModel.adjustStatusText());
}

} // namespace hlm
