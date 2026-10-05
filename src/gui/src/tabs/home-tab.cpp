#include "tabs/home-tab.h"
#include <QComboBox>
#include <QDate>
#include <QFont>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonObject>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QTimer>
#include <QVBoxLayout>
#include "image-library-actions.h"
#include "main-window.h"
#include "models/profile.h"

namespace
{
	QString pictureName(const LibraryEntry &entry)
	{
		return entry.image.value("name").toString(entry.image.value("data").toObject().value("title").toString());
	}
}

HomeTab::HomeTab(Profile *profile, MainWindow *parent)
	: QWidget(parent), m_profile(profile), m_store(profile->library()), m_recommendations(new LibraryRecommendations(profile, this))
{
	setObjectName("homeTab");
	setWindowTitle(tr("Home"));
	setMaximumWidth(16777214);
	m_collection = profile->getSettings()->value("Home/collection", 0).toLongLong();
	auto *layout = new QVBoxLayout(this);
	layout->setContentsMargins(24, 20, 24, 16);
	layout->setSpacing(12);
	auto *header = new QHBoxLayout;
	auto *title = new QLabel(tr("Discover your Library"), this);
	QFont titleFont = title->font();
	titleFont.setPointSize(22);
	titleFont.setBold(true);
	title->setFont(titleFont);
	header->addWidget(title, 1);
	auto *browse = new QPushButton(tr("Open Library"), this);
	browse->setObjectName("homeOpenLibrary");
	header->addWidget(browse);
	layout->addLayout(header);
	auto *subtitle = new QLabel(tr("From your Library · private recommendations from pictures you have already saved."), this);
	subtitle->setWordWrap(true);
	layout->addWidget(subtitle);
	auto *controls = new QHBoxLayout;
	controls->addWidget(new QLabel(tr("Preferences"), this));
	m_scope = new QComboBox(this);
	m_scope->setObjectName("homeScope");
	m_scope->setAccessibleName(tr("Recommendation preference scope"));
	m_scope->setMinimumWidth(180);
	controls->addWidget(m_scope, 1);
	m_mode = new QComboBox(this);
	m_mode->setObjectName("homeMode");
	m_mode->addItems({tr("For you"), tr("Recently saved")});
	m_mode->setAccessibleName(tr("Home picture view"));
	m_mode->setCurrentIndex(qBound(0, profile->getSettings()->value("Home/view", 0).toInt(), 1));
	controls->addWidget(m_mode);
	m_restore = new QPushButton(tr("Restore hidden suggestions"), this);
	m_restore->setObjectName("homeRestoreHidden");
	controls->addWidget(m_restore);
	layout->addLayout(controls);
	m_hint = new QLabel(this);
	m_hint->setObjectName("homeHint");
	m_hint->setTextFormat(Qt::PlainText);
	m_hint->setWordWrap(true);
	layout->addWidget(m_hint);
	auto *local = new QGroupBox(tr("Local visual recommendations"), this);
	auto *localLayout = new QVBoxLayout(local);
	auto *localRow = new QHBoxLayout;
	m_coverage = new QLabel(local);
	m_coverage->setObjectName("homeCoverage");
	m_coverage->setTextFormat(Qt::PlainText);
	m_coverage->setWordWrap(true);
	localRow->addWidget(m_coverage, 1);
	m_setup = new QPushButton(tr("Download local model"), local);
	m_setup->setObjectName("homeSetupModel");
	m_setup->setToolTip(tr("Download the visual model. Your pictures are processed on this PC and are not uploaded."));
	m_update = new QPushButton(tr("Update image index"), local);
	m_update->setObjectName("homeUpdateIndex");
	m_cancel = new QPushButton(tr("Cancel"), local);
	m_cancel->setObjectName("homeCancelIndex");
	for (auto *button : {m_setup, m_update, m_cancel}) { localRow->addWidget(button); }
	localLayout->addLayout(localRow);
	m_status = new QLabel(local);
	m_status->setObjectName("homeModelStatus");
	m_status->setTextFormat(Qt::PlainText);
	m_status->setWordWrap(true);
	localLayout->addWidget(m_status);
	m_progress = new QProgressBar(local);
	m_progress->setObjectName("homeIndexProgress");
	m_progress->setVisible(false);
	localLayout->addWidget(m_progress);
	layout->addWidget(local);
	m_grid = new QListWidget(this);
	m_grid->setObjectName("homeGrid");
	m_grid->setViewMode(QListView::IconMode);
	m_grid->setResizeMode(QListView::Adjust);
	m_grid->setMovement(QListView::Static);
	m_grid->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
	m_grid->setIconSize(QSize(224, 160));
	m_grid->setGridSize(QSize(256, 244));
	m_grid->setSpacing(10);
	m_grid->setUniformItemSizes(true);
	m_grid->setWordWrap(false);
	m_grid->setSelectionMode(QAbstractItemView::SingleSelection);
	m_grid->setTextElideMode(Qt::ElideRight);
	m_grid->setFrameShape(QFrame::NoFrame);
	layout->addWidget(m_grid, 1);
	m_empty = new QLabel(this);
	m_empty->setObjectName("homeEmpty");
	m_empty->setTextFormat(Qt::PlainText);
	m_empty->setWordWrap(true);
	m_empty->setAlignment(Qt::AlignCenter);
	m_empty->setMargin(24);
	layout->addWidget(m_empty, 1);
	m_selectionHint = new QLabel(this);
	m_selectionHint->setObjectName("homeSelectionHint");
	m_selectionHint->setTextFormat(Qt::PlainText);
	m_selectionHint->setWordWrap(true);
	layout->addWidget(m_selectionHint);
	auto *actions = new QHBoxLayout;
	m_actions = new ImageLibraryActions(profile, {}, this);
	actions->addWidget(m_actions, 1);
	m_add = new QPushButton(tr("Add to this collection"), this);
	m_add->setObjectName("homeAddToCollection");
	m_view = new QPushButton(tr("View picture"), this);
	m_view->setObjectName("homeViewPicture");
	m_hide = new QPushButton(tr("Hide suggestion"), this);
	m_hide->setObjectName("homeHideSuggestion");
	for (auto *button : {m_add, m_view, m_hide}) { actions->addWidget(button); }
	layout->addLayout(actions);
	setStyleSheet("#homeGrid::item { padding: 10px; border-radius: 8px; } #homeGrid::item:selected { background: palette(highlight); color: palette(highlighted-text); } #homeScope, #homeMode { padding: 5px; }");
	connect(browse, &QPushButton::clicked, this, [this]() { emit libraryRequested(m_collection, 0); });
	connect(m_scope, &QComboBox::currentIndexChanged, this, [this]() {
		m_collection = m_scope->currentData().toLongLong();
		m_profile->getSettings()->setValue("Home/collection", m_collection);
		scheduleReload();
	});
	connect(m_mode, &QComboBox::currentIndexChanged, this, [this]() {
		m_profile->getSettings()->setValue("Home/view", m_mode->currentIndex());
		scheduleReload();
	});
	connect(m_store, &LibraryStore::imageChanged, this, &HomeTab::scheduleReload);
	connect(m_store, &LibraryStore::collectionsChanged, this, &HomeTab::scheduleReload);
	connect(m_recommendations, &LibraryRecommendations::changed, this, &HomeTab::scheduleReload);
	connect(m_recommendations, &LibraryRecommendations::progress, this, [this](int done, int total) {
		m_progress->setRange(0, total);
		m_progress->setValue(done);
		updateModelStatus();
	});
	connect(m_setup, &QPushButton::clicked, m_recommendations, &LibraryRecommendations::downloadModel);
	connect(m_update, &QPushButton::clicked, m_recommendations, &LibraryRecommendations::startIndexing);
	connect(m_cancel, &QPushButton::clicked, m_recommendations, &LibraryRecommendations::cancel);
	connect(m_restore, &QPushButton::clicked, this, [this]() { m_recommendations->restoreHidden(m_collection); });
	connect(m_hide, &QPushButton::clicked, this, [this]() {
		if (!selectedKey().isEmpty()) { m_recommendations->hide(selectedKey(), m_collection); }
	});
	connect(m_add, &QPushButton::clicked, this, [this]() {
		const QString key = selectedKey();
		if (!key.isEmpty() && m_collection > 0 && !m_store->addToCollection(key, m_collection)) {
			QMessageBox::warning(this, tr("Library"), m_store->lastError());
		}
	});
	connect(m_grid, &QListWidget::itemSelectionChanged, this, &HomeTab::updateSelection);
	connect(m_grid, &QListWidget::itemActivated, this, [this]() { openSelected(); });
	connect(m_view, &QPushButton::clicked, this, &HomeTab::openSelected);
	reload();
}

void HomeTab::showEvent(QShowEvent *event)
{
	QWidget::showEvent(event);
	scheduleReload();
}

void HomeTab::scheduleReload()
{
	if (m_reloadPending) { return; }
	m_reloadPending = true;
	QTimer::singleShot(0, this, [this]() { m_reloadPending = false; reload(); });
}

QString HomeTab::selectedKey() const
{
	return m_grid->selectedItems().isEmpty() ? QString() : m_grid->selectedItems().first()->data(Qt::UserRole).toString();
}

void HomeTab::reload()
{
	const QString selected = selectedKey();
	const QSignalBlocker scopeBlock(m_scope);
	const QSignalBlocker gridBlock(m_grid);
	m_scope->clear();
	m_scope->addItem(tr("Library-wide"), 0);
	for (const auto &collection : m_store->collections()) { m_scope->addItem(collection.name, collection.id); }
	int index = m_scope->findData(m_collection);
	if (index < 0) {
		m_collection = 0;
		m_profile->getSettings()->setValue("Home/collection", 0);
		index = 0;
	}
	m_scope->setCurrentIndex(index);
	const bool recent = m_mode->currentIndex() == 1;
	m_restore->setVisible(!recent);
	const auto result = recent ? LibraryRecommendationResult() : m_recommendations->recommendations(m_collection);
	QList<LibraryRecommendation> items = result.items;
	if (recent) {
		for (const auto &entry : m_store->entries(m_collection).mid(0, 24)) { items.append({entry, 0, {}, {}, false}); }
	}
	m_hint->setText(recent ? tr("The latest pictures saved in this scope. Viewing or rating a picture does not change its saved date.")
		: (m_collection > 0 ? tr("Guided only by likes and favorites in %1. Favorites count three times as much as likes; close matches rotate daily.").arg(m_scope->currentText())
			: tr("Guided only by Library-wide likes and favorites. Collection preferences stay separate; favorites count three times as much as likes.")));
	m_grid->clear();
	for (const auto &recommendation : items) {
		const auto &entry = recommendation.entry;
		QString name = pictureName(entry);
		if (name.isEmpty()) { name = tr("Picture"); }
		QStringList states;
		if (entry.tags().isEmpty()) { states.append(tr("Needs tags")); }
		if (entry.image.value("website").toString().isEmpty()) { states.append(tr("Source unlinked")); }
		if (!entry.metadataErrors().isEmpty()) { states.append(tr("Metadata error")); }
		if (entry.thumbnail.isEmpty()) { states.append(tr("Preview unavailable")); }
		QString why = tr("Recently saved");
		if (!recent) {
			const auto seed = m_store->entry(recommendation.basedOnKey, m_collection);
			const QString basis = seed.favorite ? tr("favorite") : tr("liked picture");
			why = (recommendation.visual ? tr("Similar appearance to your %1: %2") : tr("Shared tags with your %1: %2")).arg(basis, pictureName(seed));
			if (!recommendation.sharedTags.isEmpty()) { why += "\n" + tr("Shared tags: %1").arg(recommendation.sharedTags.mid(0, 8).join(", ")); }
		}
		const QString badges = (entry.liked ? QStringLiteral("♥ ") : QString()) + (entry.favorite ? QStringLiteral("★ ") : QString());
		auto *item = new QListWidgetItem(name + "\n" + badges + (states.isEmpty() ? tr("Saved picture") : states.join(" · ")) + "\n" + why.section('\n', 0, 0), m_grid);
		item->setData(Qt::UserRole, entry.key);
		item->setData(Qt::UserRole + 1, why);
		item->setToolTip(name + "\n" + states.join(" · ") + "\n" + why + "\n" + entry.tags().join(", "));
		QPixmap preview;
		preview.loadFromData(entry.thumbnail);
		if (preview.isNull()) { preview.load(":/images/noimage.png"); }
		QIcon icon(preview);
		icon.addPixmap(preview, QIcon::Selected);
		item->setIcon(icon);
		item->setTextAlignment(Qt::AlignHCenter);
		if (entry.key == selected) { m_grid->setCurrentItem(item); item->setSelected(true); }
	}
	m_grid->setVisible(!items.isEmpty());
	m_empty->setVisible(items.isEmpty());
	if (!m_store->isReady()) { m_empty->setText(m_store->lastError()); }
	else if (recent) { m_empty->setText(tr("No pictures saved in this scope yet. Open Library to import pictures or organize a collection.")); }
	else if (result.ratedSeeds == 0) { m_empty->setText(tr("Make this space yours.\nLike or favorite a few pictures in %1, then return here for related pictures from your Library.").arg(m_scope->currentText())); }
	else if (result.tagSeeds == 0 && result.visualSeeds == 0) { m_empty->setText(tr("Your likes and favorites are saved, but those pictures have no tags or visual index yet.\nDownload the local model and update the image index, or recover tags in Library. Missing tags do not stop you rating pictures.")); }
	else { m_empty->setText(tr("No unrated matches in your Library yet.\nSave more pictures, update the visual index, or restore hidden suggestions for this scope.")); }
	updateModelStatus();
	updateSelection();
}

void HomeTab::updateModelStatus()
{
	const bool busy = m_recommendations->busy();
	const bool model = m_recommendations->modelAvailable();
	m_coverage->setText(tr("%1 pictures indexed · processing stays on this PC").arg(m_recommendations->indexedCount()));
	m_setup->setText(model ? tr("Reinstall local model") : tr("Download local model"));
	m_setup->setEnabled(!busy && m_store->isReady());
	m_update->setEnabled(model && !busy && m_store->isReady());
	m_cancel->setVisible(busy);
	m_progress->setVisible(busy);
	if (!busy) { m_progress->setValue(0); }
	const QString status = m_recommendations->status();
	m_status->setText(status.isEmpty() ? (model ? tr("Update the image index to include saved previews. Tags can also guide recommendations.")
		: tr("Tags can guide recommendations now. The model download needs internet; picture analysis stays local.")) : status);
}

void HomeTab::updateSelection()
{
	const QString key = selectedKey();
	const bool selected = !key.isEmpty() && m_store->contains(key);
	const bool member = selected && m_store->contains(key, m_collection);
	m_actions->setSelection({}, selected ? QStringList{key} : QStringList(), m_collection);
	m_actions->setEnabled(member);
	m_add->setVisible(selected && m_collection > 0 && !member);
	m_add->setEnabled(selected && m_collection > 0 && !member);
	m_view->setEnabled(selected);
	m_view->setText(selected && !member ? tr("View · Library-wide") : tr("View picture"));
	m_view->setToolTip(selected && !member ? tr("This picture is outside the chosen collection. Its viewer uses Library-wide preferences until you add it.") : QString());
	m_hide->setVisible(m_mode->currentIndex() == 0);
	m_hide->setEnabled(selected);
	QString hint = m_grid->selectedItems().isEmpty() ? tr("Select a picture to see why it was suggested, then like, favorite or organize it.") : m_grid->selectedItems().first()->data(Qt::UserRole + 1).toString();
	if (selected && !member) { hint += "\n" + tr("Add this picture to %1 before rating it in that collection. Its existing Library-wide ratings are kept separate.").arg(m_scope->currentText()); }
	m_selectionHint->setText(hint);
}

void HomeTab::openSelected()
{
	const QString key = selectedKey();
	if (key.isEmpty() || !m_store->contains(key)) { return; }
	const qint64 scope = m_store->contains(key, m_collection) ? m_collection : 0;
	QStringList keys;
	for (int i = 0; i < m_grid->count(); ++i) {
		const QString candidate = m_grid->item(i)->data(Qt::UserRole).toString();
		if (m_store->contains(candidate, scope)) { keys.append(candidate); }
	}
	emit pictureRequested(key, keys, scope);
}
