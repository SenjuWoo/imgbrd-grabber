#include "viewer/library-health-dialog.h"
#include <QDialogButtonBox>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPixmap>
#include <QPushButton>
#include <QSplitter>
#include <QVBoxLayout>
#include "models/library-store.h"
#include "models/profile.h"


LibraryHealthDialog::LibraryHealthDialog(Profile *profile, QWidget *parent)
	: QDialog(parent), m_profile(profile), m_store(profile->library())
{
	setObjectName("libraryHealthDialog");
	setWindowTitle(tr("Library health"));
	resize(860, 560);
	auto *layout = new QVBoxLayout(this);
	m_summary = new QLabel(this);
	m_summary->setObjectName("libraryHealthSummary");
	m_summary->setWordWrap(true);
	layout->addWidget(m_summary);
	auto *split = new QSplitter(this);
	m_issues = new QListWidget(split);
	m_issues->setObjectName("libraryHealthIssues");
	m_issues->setAccessibleName(tr("Problems found"));
	m_pictures = new QListWidget(split);
	m_pictures->setObjectName("libraryHealthPictures");
	m_pictures->setAccessibleName(tr("Affected pictures"));
	m_pictures->setIconSize(QSize(72, 72));
	m_pictures->setSelectionMode(QAbstractItemView::ExtendedSelection);
	split->setStretchFactor(0, 1);
	split->setStretchFactor(1, 2);
	layout->addWidget(split, 1);
	m_hint = new QLabel(this);
	m_hint->setWordWrap(true);
	layout->addWidget(m_hint);
	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
	m_open = buttons->addButton(tr("Open picture"), QDialogButtonBox::ActionRole);
	m_open->setObjectName("libraryHealthOpen");
	m_remove = buttons->addButton(tr("Remove from Library…"), QDialogButtonBox::ActionRole);
	m_remove->setObjectName("libraryHealthRemove");
	m_remove->setToolTip(tr("Removes the catalog entry only. Pictures on your disk are never deleted."));
	layout->addWidget(buttons);
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
	connect(m_issues, &QListWidget::currentRowChanged, this, &LibraryHealthDialog::showIssue);
	connect(m_pictures, &QListWidget::itemSelectionChanged, this, [this]() {
		m_open->setEnabled(m_pictures->selectedItems().size() == 1);
		m_remove->setEnabled(!m_pictures->selectedItems().isEmpty());
	});
	connect(m_open, &QPushButton::clicked, this, [this]() {
		const auto items = m_pictures->selectedItems();
		const auto *issue = m_health.issue(m_issues->currentItem() != nullptr ? m_issues->currentItem()->data(Qt::UserRole).toString() : QString());
		if (items.size() == 1 && issue != nullptr) {
			emit pictureRequested(items.first()->data(Qt::UserRole).toString(), issue->keys);
		}
	});
	connect(m_remove, &QPushButton::clicked, this, &LibraryHealthDialog::removeSelected);
	refresh();
}

void LibraryHealthDialog::refresh()
{
	const auto entries = m_store->isReady() ? m_store->entries() : QList<LibraryEntry>();
	m_entries.clear();
	for (const auto &entry : entries) {
		m_entries.insert(entry.key, entry);
	}
	const QStringList siteUrls = m_profile->getSites().keys();
	const QSet<QString> sites(siteUrls.cbegin(), siteUrls.cend());
	m_health = LibraryHealth::compute(entries, sites, m_store->isReady() ? m_store->duplicateGroups() : QList<QStringList>(), m_store->isReady() ? m_store->unratedKeys() : QStringList(), [](const QString &path) { return QFile::exists(path); });
	m_summary->setText(m_health.issues.isEmpty()
		? tr("%n picture(s), %1 liked, %2 favorites. Nothing to fix.", "", m_health.total).arg(m_health.liked).arg(m_health.favorites)
		: tr("%n picture(s), %1 liked, %2 favorites. %3 kind(s) of problem found. Nothing here changes a picture until you press Remove.", "", m_health.total).arg(m_health.liked).arg(m_health.favorites).arg(m_health.issues.size()));
	m_issues->clear();
	for (const auto &issue : qAsConst(m_health.issues)) {
		auto *item = new QListWidgetItem(QStringLiteral("%1 (%2)").arg(issue.title).arg(issue.keys.size()), m_issues);
		item->setData(Qt::UserRole, issue.id);
	}
	m_pictures->clear();
	m_hint->clear();
	m_open->setEnabled(false);
	m_remove->setEnabled(false);
	if (m_issues->count() > 0) {
		m_issues->setCurrentRow(0);
	}
}

void LibraryHealthDialog::showIssue(int row)
{
	m_pictures->clear();
	m_hint->clear();
	if (row < 0 || row >= m_health.issues.size()) {
		return;
	}
	const auto &issue = m_health.issues[row];
	m_hint->setText(issue.hint);
	for (const QString &key : issue.keys) {
		const auto entry = m_entries.value(key);
		QString name = entry.image.value("name").toString();
		if (name.isEmpty() && !entry.localPaths.isEmpty()) {
			name = QFileInfo(entry.localPaths.first()).fileName();
		}
		if (name.isEmpty()) {
			name = key;
		}
		auto *item = new QListWidgetItem(name + "\n" + entry.image.value("website").toString(), m_pictures);
		item->setData(Qt::UserRole, key);
		QPixmap pixels;
		pixels.loadFromData(entry.thumbnail);
		if (!pixels.isNull()) {
			item->setIcon(QIcon(pixels));
		}
	}
}

void LibraryHealthDialog::removeSelected()
{
	const auto items = m_pictures->selectedItems();
	if (items.isEmpty()) {
		return;
	}
	if (QMessageBox::question(this, tr("Remove from Library"), tr("Remove %n picture(s) from the Library catalog? Likes, favorites and notes of these entries are lost. Files on your disk stay.", "", int(items.size())), QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) {
		return;
	}
	for (auto *item : items) {
		m_store->removeImage(item->data(Qt::UserRole).toString());
	}
	emit libraryChanged();
	refresh();
}
