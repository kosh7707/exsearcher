#include "DriveChipBar.h"

#include <QHBoxLayout>
#include <QPushButton>
#include <QSettings>
#include <QStringList>

DriveChipBar::DriveChipBar(const QVector<DriveInfo>& drives,
                           QSettings* settings,
                           QWidget* parent)
    : QWidget(parent), drives_(drives), settings_(settings) {
    setObjectName("chipBar");

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);

    // Read persisted enabled drives. Default: all enabled.
    settings_->beginGroup("drives");
    const QString savedStr = settings_->value("enabled", QString()).toString();
    settings_->endGroup();

    QHash<QString, bool> savedEnabled;
    if (!savedStr.isEmpty()) {
        for (const QString& s : savedStr.split(',', Qt::SkipEmptyParts))
            savedEnabled[s.trimmed()] = true;
    }
    const bool hasSaved = !savedEnabled.isEmpty();

    for (const DriveInfo& di : drives_) {
        auto* btn = new QPushButton(this);
        // Label: "C: 로컬" / "Z: 네트워크"
        const QString label = di.letter + (di.isRemote
            ? QStringLiteral(" \U0001F310")
            : QStringLiteral(" \U0001F4BB"));
        btn->setText(label);
        btn->setCheckable(true);
        btn->setProperty("chipButton", true);
        btn->setProperty("driveLetter", di.letter);
        btn->setToolTip(di.letter + (di.isRemote ? QStringLiteral(" 네트워크 드라이브")
                                                 : QStringLiteral(" 로컬 드라이브")));

        const bool enabled = hasSaved ? savedEnabled.contains(di.letter) : true;
        btn->setChecked(enabled);

        connect(btn, &QPushButton::toggled, this, &DriveChipBar::onChipToggled);
        chips_.push_back(btn);
        layout->addWidget(btn);
    }

    layout->addStretch(1);

    reindexBtn_ = new QPushButton(QStringLiteral("재색인"), this);
    reindexBtn_->setProperty("reindexButton", true);
    reindexBtn_->setToolTip(QStringLiteral("활성화된 드라이브를 다시 색인합니다"));
    connect(reindexBtn_, &QPushButton::clicked, this, &DriveChipBar::onReindexClicked);
    layout->addWidget(reindexBtn_);
}

QVector<QString> DriveChipBar::enabledDrives() const {
    QVector<QString> result;
    for (QPushButton* btn : chips_) {
        if (btn->isChecked())
            result.push_back(btn->property("driveLetter").toString());
    }
    return result;
}

void DriveChipBar::setChipsEnabled(bool enabled) {
    for (QPushButton* btn : chips_)
        btn->setEnabled(enabled);
    reindexBtn_->setEnabled(enabled);
}

bool DriveChipBar::allEnabled() const {
    for (QPushButton* btn : chips_)
        if (!btn->isChecked())
            return false;
    return true;
}

void DriveChipBar::persistState() {
    const QVector<QString> enabled = enabledDrives();
    QStringList parts;
    for (const QString& s : enabled)
        parts.append(s);
    settings_->beginGroup("drives");
    settings_->setValue("enabled", parts.join(','));
    settings_->endGroup();
}

void DriveChipBar::onChipToggled(bool /*checked*/) {
    persistState();
    // Signal the main window — it will decide whether to re-crawl or just filter.
    emit filterChanged();
}

void DriveChipBar::onReindexClicked() {
    persistState();
    emit reindexRequested();
}
