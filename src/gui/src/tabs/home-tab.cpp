#include "tabs/home-tab.h"
#include <QComboBox>
#include <QDate>
#include <QFont>
#include <QGroupBox>
#include <QHideEvent>
#include <QRegularExpression>
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
#include "models/image.h"
#include "models/page.h"
#include "models/site.h"
#include "tags/tag.h"
#include "tabs/image-preview.h"
#include "viewer/viewer-window.h"

namespace
{
	QString pictureName(const LibraryEntry &entry)
	{
		return entry.image.value("name").toString(entry.image.value("data").toObject().value("title").toString());
	}
}

HomeTab::HomeTab(Profile *profile, MainWindow *parent)
	: QWidget(parent), m_mainWindow(parent), m_profile(profile), m_store(profile->library()), m_recommendations(new LibraryRecommendations(profile, this))
{
	setObjectName("homeTab");
	setWindowTitle(tr("Home"));
	setMaximumWidth(16777214);
	m_collection = profile->getSettings()->value("Home/collection", 0).toLongLong();
	auto *layout = new QVBoxLayout(this);
	layout->setContentsMargins(24, 20, 24, 16);
	layout->setSpacing(12);
	auto *header = new QHBoxLayout;
	auto *title = new QLabel(tr("Discover"), this);
	QFont titleFont = title->font();
	titleFont.setPointSize(22);
	titleFont.setBold(true);
	title->setFont(titleFont);
	header->addWidget(title, 1);
	auto *browse = new QPushButton(tr("Open Library"), this);
	browse->setObjectName("homeOpenLibrary");
	header->addWidget(browse);
	layout->addLayout(header);
	auto *subtitle = new QLabel(tr("Explore saved pictures privately, or discover new pictures from your selected image sources."), this);
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
	m_mode->addItems({tr("For you"), tr("Recently saved"), tr("Discover online")});
	m_mode->setAccessibleName(tr("Home picture view"));
	m_mode->setCurrentIndex(qBound(0, profile->getSettings()->value("Home/view", 0).toInt(), 2));
	controls->addWidget(m_mode);
	m_restore = new QPushButton(tr("Restore hidden suggestions"), this);
	m_restore->setObjectName("homeRestoreHidden");
	controls->addWidget(m_restore);
	layout->addLayout(controls);
	auto *display = new QHBoxLayout;
	display->addWidget(new QLabel(tr("Density"), this));
	m_density = new QComboBox(this);
	m_density->setObjectName("homeDensity");
	m_density->setAccessibleName(tr("Picture density"));
	m_density->addItems({tr("Compact"), tr("Comfortable"), tr("Large")});
	m_density->setCurrentIndex(qBound(0, profile->getSettings()->value("Gallery/density", 1).toInt(), 2));
	display->addWidget(m_density);
	display->addWidget(new QLabel(tr("Pictures"), this));
	m_pictureCount = new QComboBox(this);
	m_pictureCount->setObjectName("homePictureCount");
	m_pictureCount->setAccessibleName(tr("Pictures shown on Home"));
	for (const int count : {24, 48, 96}) { m_pictureCount->addItem(QString::number(count), count); }
	const int countIndex = m_pictureCount->findData(profile->getSettings()->value("Home/pictureCount", 48).toInt());
	m_pictureCount->setCurrentIndex(countIndex < 0 ? 1 : countIndex);
	display->addWidget(m_pictureCount);
	display->addStretch();
	m_refresh = new QPushButton(tr("Refresh suggestions"), this);
	m_refresh->setObjectName("homeRefreshSuggestions");
	m_refresh->setToolTip(tr("Rotate close matches in this preference scope while keeping recommendations relevant."));
	display->addWidget(m_refresh);
	layout->addLayout(display);
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
	m_grid->setSpacing(6);
	m_grid->setUniformItemSizes(true);
	m_grid->setWordWrap(false);
	m_grid->setSelectionMode(QAbstractItemView::SingleSelection);
	m_grid->setTextElideMode(Qt::ElideRight);
	m_grid->setFrameShape(QFrame::NoFrame);
	layout->addWidget(m_grid, 1);
	updateDensity();
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
	setStyleSheet("#homeGrid::item { padding: 4px; border-radius: 5px; } #homeGrid::item:selected { background: palette(highlight); color: palette(highlighted-text); } #homeScope, #homeMode { padding: 5px; }");
	connect(browse, &QPushButton::clicked, this, [this]() { emit libraryRequested(m_collection, 0); });
	connect(m_scope, &QComboBox::currentIndexChanged, this, [this]() {
		m_collection = m_scope->currentData().toLongLong();
		m_profile->getSettings()->setValue("Home/collection", m_collection);
		scheduleReload();
	});
	connect(m_mode, &QComboBox::currentIndexChanged, this, [this]() {
		cancelDiscovery();
		m_discoverySession.clear();
		m_profile->getSettings()->setValue("Home/view", m_mode->currentIndex());
		scheduleReload();
	});
	connect(m_density, &QComboBox::currentIndexChanged, this, [this]() {
		m_profile->getSettings()->setValue("Gallery/density", m_density->currentIndex());
		updateDensity();
	});
	connect(m_pictureCount, &QComboBox::currentIndexChanged, this, [this]() {
		m_profile->getSettings()->setValue("Home/pictureCount", m_pictureCount->currentData().toInt());
		scheduleReload();
	});
	connect(m_refresh, &QPushButton::clicked, this, [this]() {
		++m_rotations[m_collection];
		m_grid->clearSelection();
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
		QString key = selectedKey();
		const auto image = discoveredImage(key);
		if (image) {
			key = m_store->saveImage(*image);
			if (key.isEmpty()) { QMessageBox::warning(this, tr("Library"), m_store->lastError()); return; }
		}
		if (!key.isEmpty() && m_collection > 0 && !m_store->addToCollection(key, m_collection)) {
			QMessageBox::warning(this, tr("Library"), m_store->lastError());
		}
	});
	connect(m_grid, &QListWidget::itemSelectionChanged, this, &HomeTab::updateSelection);
	connect(m_grid, &QListWidget::itemActivated, this, [this]() { openSelected(); });
	connect(m_view, &QPushButton::clicked, this, &HomeTab::openSelected);

	m_discoveryTimeout = new QTimer(this);
	m_discoveryTimeout->setSingleShot(true);
	connect(m_discoveryTimeout, &QTimer::timeout, this, [this]() {
		m_discoveryErrors.append(tr("Some sources or previews timed out. Refresh to try again; successful results are kept."));
		cancelDiscovery();
		scheduleReload();
	});
	reload();
}

HomeTab::~HomeTab() { cancelDiscovery(); }

void HomeTab::hideEvent(QHideEvent *event)
{
	QWidget::hideEvent(event);
	if (!m_discoveryPages.isEmpty() || !m_previewLoaders.isEmpty()) {
		cancelDiscovery();
		m_discoverySession.clear();
	}
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
	const bool online = m_mode->currentIndex() == 2;
	const bool recent = m_mode->currentIndex() == 1;
	m_restore->setVisible(!recent);
	m_refresh->setVisible(!recent);
	const int limit = m_pictureCount->currentData().toInt();
	const auto result = recent || online ? LibraryRecommendationResult() : m_recommendations->recommendations(m_collection, QDate::currentDate(), limit, m_rotations.value(m_collection));
	m_refresh->setEnabled(!result.items.isEmpty());
	const QSignalBlocker densityBlock(m_density);
	m_density->setCurrentIndex(qBound(0, m_profile->getSettings()->value("Gallery/density", 1).toInt(), 2));
	updateDensity();

	if (online) {
		const QString session = QString::number(m_collection) + ':' + QDate::currentDate().toString(Qt::ISODate) + ':'
			+ QString::number(m_rotations.value(m_collection)) + ':' + QString::number(qMin(48, m_pictureCount->currentData().toInt())) + ':'
			+ m_profile->getSettings()->value("sites").toStringList().join('|');
		if (m_discoverySession != session) { startDiscovery(session); }
		showDiscovery(selected);
		updateModelStatus();
		updateSelection();
		return;
	}
	QList<LibraryRecommendation> items = result.items;
	if (recent) {
		for (const auto &entry : m_store->entries(m_collection).mid(0, limit)) { items.append({entry, 0, {}, {}, false}); }
	}
	m_hint->setText(recent ? tr("The latest pictures saved in this scope. Viewing or rating a picture does not change its saved date.")
		: (m_collection > 0 ? tr("Guided only by likes and favorites in %1. Favorites count three times as much as likes. Different tastes are balanced; close matches rotate daily or when refreshed.").arg(m_scope->currentText())
			: tr("Guided only by Library-wide likes and favorites. Collection preferences stay separate; favorites count three times as much as likes. Refresh to rotate close matches.")));
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
		auto *item = new QListWidgetItem(m_grid);
		const QString details = name + "\n" + why + (states.isEmpty() ? QString() : "\n" + states.join(" · "));
		item->setData(Qt::UserRole, entry.key);
		item->setData(Qt::UserRole + 1, details);
		item->setData(Qt::AccessibleTextRole, name);
		item->setData(Qt::AccessibleDescriptionRole, details);
		item->setToolTip(details + "\n" + entry.tags().join(", "));
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
	else { m_empty->setText(tr("No unrated matches in your Library yet.\nChoose Discover online to find new pictures from your selected sources, save more pictures, or restore hidden suggestions.")); }
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
	const auto image = discoveredImage(key);
	const bool selected = !key.isEmpty() && (m_store->contains(key) || image);
	const bool member = selected && (m_collection == 0 || m_store->contains(key, m_collection));
	m_actions->setSelection(image ? QList<QSharedPointer<Image>>{image} : QList<QSharedPointer<Image>>{}, selected ? QStringList{key} : QStringList(), m_collection);
	m_actions->setEnabled(member);
	m_actions->setVisible(selected);
	m_add->setVisible(selected && m_collection > 0 && !member);
	m_add->setEnabled(selected && m_collection > 0 && !member);
	m_view->setEnabled(selected);
	m_view->setVisible(selected);
	m_view->setText(selected && !member ? tr("View · Library-wide") : tr("View picture"));
	m_view->setToolTip(selected && !member ? tr("This picture is outside the chosen collection. Its viewer uses Library-wide preferences until you add it.") : QString());
	m_hide->setVisible(selected && m_mode->currentIndex() != 1);
	m_hide->setEnabled(selected);
	QString hint = m_grid->selectedItems().isEmpty() ? tr("Select a picture to see why it was suggested, then like, favorite or organize it.") : m_grid->selectedItems().first()->data(Qt::UserRole + 1).toString();
	if (selected && !member) { hint += "\n" + tr("Add this picture to %1 before rating it in that collection. Its existing Library-wide ratings are kept separate.").arg(m_scope->currentText()); }
	m_selectionHint->setText(hint);
}

void HomeTab::updateDensity()
{
	const int size = m_density->currentIndex() == 0 ? 128 : m_density->currentIndex() == 2 ? 256 : 180;
	m_grid->setIconSize(QSize(size, size));
	m_grid->setGridSize(QSize(size + 12, size + 12));
}

void HomeTab::openSelected()
{
	const QString key = selectedKey();

	if (const auto image = discoveredImage(key)) {
		const qint64 scope = m_collection > 0 && m_store->contains(key, m_collection) ? m_collection : 0;
		QList<QSharedPointer<Image>> images;
		for (const auto &candidate : m_discoveryImages) {
			if (scope == 0 || m_store->contains(m_store->keyForImage(*candidate), scope)) { images.append(candidate); }
		}
		auto *viewer = new ViewerWindow(images, image, image->parentSite(), m_profile, m_mainWindow, nullptr, scope);
		viewer->go();
		return;
	}
	if (key.isEmpty() || !m_store->contains(key)) { return; }
	const qint64 scope = m_store->contains(key, m_collection) ? m_collection : 0;
	QStringList keys;
	for (int i = 0; i < m_grid->count(); ++i) {
		const QString candidate = m_grid->item(i)->data(Qt::UserRole).toString();
		if (m_store->contains(candidate, scope)) { keys.append(candidate); }
	}
	emit pictureRequested(key, keys, scope);
}

void HomeTab::cancelDiscovery()
{
	++m_discoveryGeneration;
	m_discoveryTimeout->stop();
	const auto pages = m_discoveryPages.keys();
	m_discoveryPages.clear();
	for (auto *page : pages) {
		disconnect(page, nullptr, this, nullptr);
		page->abort();
		page->abortTags();
		page->deleteLater();
	}
	for (const auto &loader : m_previewLoaders) {
		if (loader) { disconnect(loader, nullptr, this, nullptr); loader->abort(); loader->deleteLater(); }
	}
	for (const auto &container : m_previewContainers) { if (container) { container->deleteLater(); } }
	m_previewLoaders.clear();
	m_previewContainers.clear();
}

QSharedPointer<Image> HomeTab::discoveredImage(const QString &key) const
{
	for (const auto &image : m_discoveryImages) { if (m_store->keyForImage(*image) == key) { return image; } }
	return {};
}

void HomeTab::startDiscovery(const QString &session)
{
	cancelDiscovery();
	m_discoverySession = session;
	m_discoveryImages.clear();
	m_discoveryReasons.clear();
	m_discoveryErrors.clear();
	const auto sources = m_profile->getSettings()->value("sites").toStringList();
	const int count = qMin(48, m_pictureCount->currentData().toInt());
	const auto topics = LibraryRecommender::topics(m_store->entries(m_collection), sources, m_collection,
		QDate::currentDate(), (count + 15) / 16, m_rotations.value(m_collection));
	// Register the full batch before starting requests: cached failures can finish immediately.
	for (const auto &topic : topics) {
		auto *site = m_profile->getSites().value(topic.website);
		if (site == nullptr) { continue; }
		const int pageNumber = 1 + static_cast<int>(m_rotations.value(m_collection) % 4);
		auto *page = new Page(m_profile, site, m_profile->getSites().values(), QStringList{topic.tag}, pageNumber, 16, {}, false, this);
		m_discoveryPages.insert(page, topic);
		connect(page, &Page::finishedLoading, this, [this](Page *result) { finishDiscovery(result, true); });
		connect(page, &Page::failedLoading, this, [this](Page *result) { finishDiscovery(result, false); });
		connect(page, &Page::httpsRedirect, this, [this](Page *result) { finishDiscovery(result, false); });
	}
	m_discoveryHadTopics = !m_discoveryPages.isEmpty();
	if (m_discoveryHadTopics) { m_discoveryTimeout->start(40000); }
	for (auto *page : m_discoveryPages.keys()) { page->load(); }
}

void HomeTab::finishDiscovery(Page *page, bool success)
{
	if (!m_discoveryPages.contains(page)) { return; }
	const auto topic = m_discoveryPages.take(page);
	if (!success) { m_discoveryErrors.append(tr("%1 could not load this topic. Refresh to retry.").arg(topic.website)); }
	QSet<QString> knownKeys;
	QSet<QString> knownHashes;
	static const QRegularExpression md5Pattern("^[a-fA-F0-9]{32}$");
	for (const auto &entry : m_store->entries()) {
		knownKeys.insert(entry.key);
		const QString hash = entry.image.value("md5").toString();
		if (md5Pattern.match(hash).hasMatch()) { knownHashes.insert(hash.toLower()); }
	}
	for (const auto &image : m_discoveryImages) {
		knownKeys.insert(m_store->keyForImage(*image));
		if (md5Pattern.match(image->md5()).hasMatch()) { knownHashes.insert(image->md5().toLower()); }
	}
	const auto hidden = m_recommendations->hiddenKeys(m_collection);
	if (success) {
		for (const auto &sourceImage : page->images().mid(0, 16)) {
			if (!sourceImage || !sourceImage->isValid()) { continue; }
			if (m_profile->getSettings()->value("hideblacklisted", false).toBool()
				&& !m_profile->getBlacklist().match(sourceImage->tokens(m_profile)).isEmpty()) { continue; }
			QJsonObject data;
			sourceImage->write(data);
			// A viewer can outlive a refresh. Serialization deliberately drops Page ownership.
			auto image = QSharedPointer<Image>::create(m_profile);
			if (!image->read(data, m_profile->getSites())) { continue; }
			const QString key = m_store->keyForImage(*image);
			const QString hash = image->md5().toLower();
			if (key.isEmpty() || knownKeys.contains(key) || hidden.contains(key)
				|| (md5Pattern.match(hash).hasMatch() && knownHashes.contains(hash))) { continue; }
			knownKeys.insert(key);
			if (md5Pattern.match(hash).hasMatch()) { knownHashes.insert(hash); }
			m_discoveryImages.append(image);
			m_discoveryReasons.insert(key, tr("Searched %1 on %2, guided by your ratings in %3.\nThis is a tag search; the preview has not been analyzed by local AI.")
				.arg(topic.tag, topic.website, m_scope->currentText()));
			auto *container = new QWidget(this);
			container->hide();
			auto *loader = new ImagePreview(image, container, m_profile, nullptr, m_mainWindow, this);
			m_previewLoaders.append(loader);
			m_previewContainers.append(container);
			const quint64 generation = m_discoveryGeneration;
			connect(loader, &ImagePreview::finished, this, [this, loader, container, generation]() {
				m_previewLoaders.removeAll(loader);
				m_previewContainers.removeAll(container);
				loader->deleteLater();
				container->deleteLater();
				if (generation != m_discoveryGeneration) { return; }
				if (m_discoveryPages.isEmpty() && m_previewLoaders.isEmpty()) { m_discoveryTimeout->stop(); }
				scheduleReload();
			});
			loader->load();
		}
	}
	disconnect(page, nullptr, this, nullptr);
	page->deleteLater();
	if (m_discoveryPages.isEmpty() && m_previewLoaders.isEmpty()) { m_discoveryTimeout->stop(); }
	scheduleReload();
}

void HomeTab::showDiscovery(const QString &selected)
{
	m_grid->clear();
	const auto hidden = m_recommendations->hiddenKeys(m_collection);
	const int limit = qMin(48, m_pictureCount->currentData().toInt());
	for (const auto &image : m_discoveryImages) {
		const QString key = m_store->keyForImage(*image);
		const auto state = m_store->entry(key, m_collection);
		if (hidden.contains(key) || state.liked || state.favorite || m_grid->count() >= limit) { continue; }
		const QString name = image->name().isEmpty() ? tr("Picture #%1").arg(image->id()) : image->name();
		QStringList tags;
		for (const auto &tag : image->tags()) { tags.append(tag.text()); }
		QString details = name + "\n" + m_discoveryReasons.value(key);
		if (tags.isEmpty()) { details += "\n" + tr("Tags unavailable from this source. You can still like or favorite this picture."); }
		if (image->previewImage().isNull()) { details += "\n" + tr("Preview unavailable or still loading. Open the picture or refresh to retry."); }
		auto *item = new QListWidgetItem(m_grid);
		item->setData(Qt::UserRole, key);
		item->setData(Qt::UserRole + 1, details);
		item->setData(Qt::AccessibleTextRole, name);
		item->setData(Qt::AccessibleDescriptionRole, details);
		item->setToolTip(details + "\n" + tags.join(", "));
		QPixmap preview = image->previewImage();
		if (preview.isNull()) { preview.load(":/images/noimage.png"); }
		QIcon icon(preview);
		icon.addPixmap(preview, QIcon::Selected);
		item->setIcon(icon);
		if (key == selected) { m_grid->setCurrentItem(item); item->setSelected(true); }
	}
	const bool loading = !m_discoveryPages.isEmpty();
	m_grid->setVisible(m_grid->count() > 0);
	m_empty->setVisible(m_grid->count() == 0);
	m_refresh->setEnabled(true);
	m_hint->setText(tr("Online discovery searches your selected sources using real tags from ratings in %1. Favorites count three times as much as likes. Pictures stay unsaved until you use a Library action. Up to 48 candidates per refresh.")
		.arg(m_scope->currentText()) + (loading ? "\n" + tr("Loading %n source search(es)…", "", m_discoveryPages.size()) : QString())
		+ (m_discoveryErrors.isEmpty() ? QString() : "\n" + m_discoveryErrors.join("\n")));
	if (loading) { m_empty->setText(tr("Finding new pictures from your selected sources…")); }
	else if (m_profile->getSettings()->value("sites").toStringList().isEmpty()) {
		m_empty->setText(tr("Select image sources in Search first, then refresh discovery. Your current likes and favorites are kept."));
	} else if (!m_discoveryHadTopics && m_discoveryErrors.isEmpty()) {
		m_empty->setText(tr("Like or favorite tagged pictures from your selected sources in %1, then refresh. Tagless ratings remain saved; online discovery needs source tags, while local visual recommendations can use their previews.").arg(m_scope->currentText()));
	} else { m_empty->setText(tr("No new pictures from these topics. Refresh to try different topics or pages, select more sources, or restore hidden suggestions.")); }
}
