#include "DriveChipBar.h"

#include <QHBoxLayout>
#include <QMenu>
#include <QPushButton>
#include <QSettings>
#include <QStringList>
#include <QStyle>

DriveChipBar::DriveChipBar(const QVector<DriveInfo>& drives,
                           QSettings* settings,
                           QWidget* parent)
    : QWidget(parent), drives_(drives), settings_(settings) {
    setObjectName("chipBar");

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);

    // Read persisted checked drives. Default: all checked.
    settings_->beginGroup("drives");
    const QString savedStr = settings_->value("enabled", QString()).toString();
    settings_->endGroup();

    QHash<QString, bool> savedChecked;
    if (!savedStr.isEmpty()) {
        for (const QString& s : savedStr.split(',', Qt::SkipEmptyParts))
            savedChecked[s.trimmed()] = true;
    }
    const bool hasSaved = !savedChecked.isEmpty();

    for (const DriveInfo& di : drives_) {
        auto* btn = new QPushButton(this);
        const QString label = di.letter + (di.isRemote
            ? QStringLiteral(" 네트워크")
            : QStringLiteral(" 로컬"));
        btn->setText(label);
        btn->setCheckable(false);  // we manage checked state via properties
        btn->setProperty("chipButton", true);
        btn->setProperty("driveLetter", di.letter);
        btn->setContextMenuPolicy(Qt::CustomContextMenu);

        checked_[di.letter] = hasSaved ? savedChecked.contains(di.letter) : true;

        connect(btn, &QPushButton::clicked, this, &DriveChipBar::onChipClicked);
        connect(btn, &QPushButton::customContextMenuRequested, this,
                &DriveChipBar::onChipContextMenu);
        chips_.push_back(btn);
        layout->addWidget(btn);
        restyleChip(btn);
    }

    layout->addStretch(1);

    reindexBtn_ = new QPushButton(QStringLiteral("재색인"), this);
    reindexBtn_->setProperty("reindexButton", true);
    reindexBtn_->setToolTip(QStringLiteral("색인된 드라이브를 모두 다시 색인합니다"));
    connect(reindexBtn_, &QPushButton::clicked, this,
            &DriveChipBar::onReindexClicked);
    layout->addWidget(reindexBtn_);
}

void DriveChipBar::restyleChip(QPushButton* btn) {
    const QString letter = btn->property("driveLetter").toString();
    const bool isIndexed = indexed_.contains(letter);
    const bool isChecked = checked_.value(letter, true);

    // Dynamic properties drive the stylesheet selectors.
    btn->setProperty("indexed", isIndexed);
    btn->setProperty("checked", isIndexed && isChecked);

    QString tip = letter;
    // Find drive remoteness for the tooltip text.
    for (const DriveInfo& di : drives_) {
        if (di.letter == letter) {
            tip += di.isRemote ? QStringLiteral(" 네트워크 드라이브")
                               : QStringLiteral(" 로컬 드라이브");
            break;
        }
    }
    if (!isIndexed)
        tip += QStringLiteral(" — 미색인 (클릭하여 색인)");
    else if (!isChecked)
        tip += QStringLiteral(" — 색인됨 (검색에서 제외)");
    else
        tip += QStringLiteral(" — 색인됨");
    btn->setToolTip(tip);

    // Re-polish so the new dynamic-property values take effect.
    btn->style()->unpolish(btn);
    btn->style()->polish(btn);
    btn->update();
}

void DriveChipBar::setIndexedDrives(const QSet<QString>& indexed) {
    indexed_ = indexed;
    for (QPushButton* btn : chips_)
        restyleChip(btn);
}

QVector<QString> DriveChipBar::enabledDrives() const {
    QVector<QString> result;
    for (QPushButton* btn : chips_) {
        const QString letter = btn->property("driveLetter").toString();
        if (indexed_.contains(letter) && checked_.value(letter, true))
            result.push_back(letter);
    }
    return result;
}

void DriveChipBar::setChipsEnabled(bool enabled) {
    for (QPushButton* btn : chips_)
        btn->setEnabled(enabled);
    reindexBtn_->setEnabled(enabled && anyIndexed());
}

bool DriveChipBar::allIndexedEnabled() const {
    for (QPushButton* btn : chips_) {
        const QString letter = btn->property("driveLetter").toString();
        if (indexed_.contains(letter) && !checked_.value(letter, true))
            return false;
    }
    return true;
}

void DriveChipBar::persistState() {
    QStringList parts;
    for (QPushButton* btn : chips_) {
        const QString letter = btn->property("driveLetter").toString();
        if (checked_.value(letter, true))
            parts.append(letter);
    }
    settings_->beginGroup("drives");
    settings_->setValue("enabled", parts.join(','));
    settings_->endGroup();
}

void DriveChipBar::onChipClicked() {
    auto* btn = qobject_cast<QPushButton*>(sender());
    if (!btn)
        return;
    const QString letter = btn->property("driveLetter").toString();

    if (!indexed_.contains(letter)) {
        // Not indexed: clicking crawls this drive.
        emit crawlRequested(letter);
        return;
    }

    // Indexed: toggle the search filter checked state.
    const bool now = !checked_.value(letter, true);
    checked_[letter] = now;
    persistState();
    restyleChip(btn);
    emit filterChanged();
}

void DriveChipBar::onChipContextMenu(const QPoint& pos) {
    auto* btn = qobject_cast<QPushButton*>(sender());
    if (!btn)
        return;
    const QString letter = btn->property("driveLetter").toString();
    if (!indexed_.contains(letter))
        return;  // nothing to do for a non-indexed drive

    QMenu menu(this);
    QAction* recrawl = menu.addAction(QStringLiteral("재색인"));
    QAction* remove = menu.addAction(QStringLiteral("색인에서 제거"));
    QAction* chosen = menu.exec(btn->mapToGlobal(pos));
    if (chosen == recrawl)
        emit recrawlDriveRequested(letter);
    else if (chosen == remove)
        emit removeDriveRequested(letter);
}

void DriveChipBar::onReindexClicked() {
    emit reindexAllRequested();
}
