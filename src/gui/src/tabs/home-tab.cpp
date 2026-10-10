#include "tabs/home-tab.h"
#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include "discovery-feed.h"
#include "main-window.h"
#include "models/image.h"
#include "models/library-recommendations.h"
#include "models/library-store.h"
#include "models/profile.h"
#include "models/site.h"
#include "picture-actions.h"
#include "ui/toast.h"
#include "viewer/viewer-window.h"

namespace
{
	QString blend(const QColor &a, const QColor &b, double amount)
	{
		const QColor mixed(int(a.red() * (1 - amount) + b.red() * amount), int(a.green() * (1 - amount) + b.green() * amount), int(a.blue() * (1 - amount) + b.blue() * amount));
		return mixed.name();
	}

	QString pictureTitle(const QSharedPointer<Image> &image)
	{
		if (!image->name().isEmpty()) {
			return image->name();
		}
		const QString site = image->parentSite() != nullptr ? image->parentSite()->name() : QString();
		return HomeTab::tr("Picture %1 · %2").arg(image->id()).arg(site);
	}
}

HomeTab::HomeTab(Profile *profile, MainWindow *parent, DownloadQueue *downloadQueue)
	: QWidget(parent), m_profile(profile), m_mainWindow(parent), m_downloadQueue(downloadQueue), m_store(profile->library()),
	  m_recommendations(new LibraryRecommendations(profile, this)), m_feed(new DiscoveryFeed(profile, m_recommendations, this))
{
	setObjectName("homeTab");
	setWindowTitle(tr("Discover"));
	setMaximumWidth(16777214); // Existing convention for permanent tabs.
	QSettings *settings = profile->getSettings();

	auto *layout = new QVBoxLayout(this);
	layout->setContentsMargins(24, 18, 24, 10);
	layout->setSpacing(10);

	auto *header = new QHBoxLayout;
	auto *titles = new QVBoxLayout;
	titles->setSpacing(2);
	auto *title = new QLabel(tr("Discover"), this);
	title->setObjectName("pageTitle");
	m_subtitle = new QLabel(this);
	m_subtitle->setObjectName("pageSubtitle");
	m_subtitle->setTextFormat(Qt::PlainText);
	m_subtitle->setWordWrap(true);
	titles->addWidget(title);
	titles->addWidget(m_subtitle);
	header->addLayout(titles, 1);
	m_scope = new QComboBox(this);
	m_scope->setObjectName("homeScope");
	m_scope->setAccessibleName(tr("Taste to discover with"));
	m_scope->setToolTip(tr("Use every like and favorite, or only one collection's taste."));
	m_scope->setMinimumWidth(170);
	m_refresh = new QPushButton(tr("↻  Refresh"), this);
	m_refresh->setObjectName("homeRefresh");
	m_refresh->setToolTip(tr("Start a fresh feed with new topics (F5)"));
	m_refresh->setShortcut(QKeySequence(Qt::Key_F5));
	m_more = new QToolButton(this);
	m_more->setObjectName("homeMore");
	m_more->setText(QStringLiteral("⋯"));
	m_more->setToolTip(tr("Discover options"));
	m_more->setAccessibleName(tr("Discover options"));
	m_more->setPopupMode(QToolButton::InstantPopup);
	for (QWidget *widget : {static_cast<QWidget*>(m_scope), static_cast<QWidget*>(m_refresh), static_cast<QWidget*>(m_more)}) {
		header->addWidget(widget, 0, Qt::AlignVCenter);
	}
	layout->addLayout(header);

	m_aiBanner = new QFrame(this);
	m_aiBanner->setObjectName("homeAiBanner");
	auto *bannerLayout = new QHBoxLayout(m_aiBanner);
	bannerLayout->setContentsMargins(14, 10, 10, 10);
	auto *sparkle = new QLabel(QStringLiteral("✨"), m_aiBanner);
	sparkle->setObjectName("homeAiIcon");
	m_aiText = new QLabel(m_aiBanner);
	m_aiText->setObjectName("homeAiText");
	m_aiText->setWordWrap(true);
	m_aiText->setTextFormat(Qt::RichText);
	m_aiProgress = new QProgressBar(m_aiBanner);
	m_aiProgress->setObjectName("homeAiProgress");
	m_aiProgress->setTextVisible(false);
	m_aiProgress->setFixedWidth(140);
	m_aiProgress->setFixedHeight(6);
	m_aiEnable = new QPushButton(tr("Turn on"), m_aiBanner);
	m_aiEnable->setObjectName("homeAiEnable");
	auto *dismiss = new QPushButton(tr("Not now"), m_aiBanner);
	dismiss->setObjectName("homeAiDismiss");
	dismiss->setFlat(true);
	bannerLayout->addWidget(sparkle);
	bannerLayout->addWidget(m_aiText, 1);
	bannerLayout->addWidget(m_aiProgress);
	bannerLayout->addWidget(m_aiEnable);
	bannerLayout->addWidget(dismiss);
	layout->addWidget(m_aiBanner);

	m_chips = new QWidget(this);
	m_chips->setObjectName("homeChips");
	m_chipsLayout = new QHBoxLayout(m_chips);
	m_chipsLayout->setContentsMargins(0, 0, 0, 0);
	m_chipsLayout->setSpacing(6);
	layout->addWidget(m_chips);

	m_busy = new QProgressBar(this);
	m_busy->setObjectName("homeBusy");
	m_busy->setRange(0, 0);
	m_busy->setTextVisible(false);
	m_busy->setFixedHeight(3);
	m_busy->setVisible(false);
	layout->addWidget(m_busy);

	m_stack = new QStackedWidget(this);
	m_grid = new ImageGridView(m_stack);
	m_grid->setObjectName("homeGrid");
	m_grid->setActions({ImageGridView::Like, ImageGridView::Favorite, ImageGridView::Download, ImageGridView::Hide});
	m_grid->setDensity(settings->value("Gallery/density", 1).toInt());
	m_grid->setAccessibleName(tr("Discovered pictures"));
	m_empty = new QLabel(m_stack);
	m_empty->setObjectName("homeEmpty");
	m_empty->setAlignment(Qt::AlignCenter);
	m_empty->setWordWrap(true);
	m_empty->setTextFormat(Qt::PlainText);
	m_empty->setMargin(40);
	m_stack->addWidget(m_grid);
	m_stack->addWidget(m_empty);
	m_stack->setCurrentWidget(m_empty);
	layout->addWidget(m_stack, 1);
	m_status = new QLabel(this);
	m_status->setObjectName("homeStatus");
	m_status->setTextFormat(Qt::PlainText);
	layout->addWidget(m_status);

	const QPalette colors = palette();
	const QString muted = blend(colors.color(QPalette::WindowText), colors.color(QPalette::Window), 0.4);
	const QColor accent = colors.color(QPalette::Highlight);
	setStyleSheet(QStringLiteral(
		"#pageTitle { font-size: 22pt; font-weight: 700; }"
		"#pageSubtitle, #homeStatus { color: %1; }"
		"#homeEmpty { font-size: 12pt; color: %1; }"
		"#homeRefresh, #homeMore, #homeChips QPushButton { border-radius: 15px; padding: 6px 14px; }"
		"#homeChips QPushButton { padding: 4px 12px; }"
		"#homeAiBanner { border-radius: 12px; background: rgba(%2, %3, %4, 30); border: 1px solid rgba(%2, %3, %4, 90); }"
		"#homeAiIcon { font-size: 16pt; background: transparent; }"
		"#homeAiText { background: transparent; }"
		"#homeBusy { border: 0; background: transparent; }"
		"#homeBusy::chunk { background: rgb(%2, %3, %4); }"
	).arg(muted).arg(accent.red()).arg(accent.green()).arg(accent.blue()));

	auto *menu = new QMenu(m_more);
	connect(menu, &QMenu::aboutToShow, this, [this, menu]() {
		menu->clear();
		auto *density = menu->addMenu(tr("Picture size"));
		const int current = m_profile->getSettings()->value("Gallery/density", 1).toInt();
		const QStringList names {tr("Compact"), tr("Comfortable"), tr("Large")};
		for (int i = 0; i < names.size(); ++i) {
			auto *action = density->addAction(names[i], this, [this, i]() {
				m_profile->getSettings()->setValue("Gallery/density", i);
				m_grid->setDensity(i);
			});
			action->setCheckable(true);
			action->setChecked(current == i);
		}
		auto *ai = menu->addAction(tr("Smart visual matching (local AI)"), this, [this](bool checked) {
			if (checked) {
				enableAi();
			} else {
				m_profile->getSettings()->setValue("Discover/visual", false);
				m_feed->setVisualEnabled(false);
				updateAi();
				updateHeader();
			}
		});
		ai->setCheckable(true);
		ai->setChecked(m_profile->getSettings()->value("Discover/visual", false).toBool());
		ai->setEnabled(!m_recommendations->busy());
		if (m_recommendations->modelAvailable()) {
			menu->addAction(tr("Update visual index (%n picture(s))", "", m_recommendations->indexedCount()), m_recommendations, &LibraryRecommendations::startIndexing)->setEnabled(!m_recommendations->busy());
		}
		menu->addSeparator();
		menu->addAction(tr("Show hidden pictures again (%1)").arg(m_feed->dismissedCount()), this, [this]() {
			m_feed->resetDismissed();
			Toast::show(this, tr("Hidden pictures can appear again."));
		})->setEnabled(m_feed->dismissedCount() > 0);
		menu->addAction(tr("Open Library"), this, [this]() { emit libraryRequested(m_feed->scope(), 0); });
	});
	m_more->setMenu(menu);

	connect(m_refresh, &QPushButton::clicked, this, &HomeTab::refresh);
	connect(m_scope, &QComboBox::currentIndexChanged, this, [this]() {
		const qint64 scope = m_scope->currentData().toLongLong();
		m_profile->getSettings()->setValue("Home/collection", scope);
		m_feed->setScope(scope);
		if (m_started) {
			refresh();
		}
	});
	connect(m_aiEnable, &QPushButton::clicked, this, &HomeTab::enableAi);
	connect(dismiss, &QPushButton::clicked, this, [this]() {
		m_profile->getSettings()->setValue("Discover/aiBannerDismissed", true);
		updateAi();
	});
	connect(m_grid, &ImageGridView::nearEnd, this, [this]() {
		if (m_started && !m_feed->isBusy() && m_grid->gridModel()->rowCount() > 0) {
			m_feed->fetchMore();
		}
	});
	connect(m_grid, &ImageGridView::actionTriggered, this, &HomeTab::triggerAction);
	connect(m_grid, &ImageGridView::openRequested, this, &HomeTab::openPicture);
	connect(m_grid, &ImageGridView::contextMenuRequested, this, &HomeTab::showMenu);
	connect(m_feed, &DiscoveryFeed::itemsReady, this, &HomeTab::addItems);
	connect(m_feed, &DiscoveryFeed::busyChanged, this, [this](bool busy) {
		m_busy->setVisible(busy);
		if (!busy && m_feed->lastBatchProductive()) {
			// Keep filling until the screen is full; later batches load as you scroll.
			QTimer::singleShot(0, m_grid, &ImageGridView::checkNearEnd);
		}
		if (busy && m_grid->gridModel()->rowCount() == 0) {
			m_empty->setText(tr("Finding pictures you'll like…"));
			m_stack->setCurrentWidget(m_empty);
		}
	});
	connect(m_feed, &DiscoveryFeed::statusChanged, this, [this](const QString &status) {
		m_status->setText(status);
		if (!m_feed->isBusy() && m_grid->gridModel()->rowCount() == 0) {
			m_empty->setText(status.isEmpty() ? tr("Nothing new right now. Try Refresh or add more sources.") : status);
			m_stack->setCurrentWidget(m_empty);
		}
	});
	connect(m_store, &LibraryStore::imageChanged, this, &HomeTab::syncRating);
	connect(m_store, &LibraryStore::collectionsChanged, this, &HomeTab::updateScopes);
	connect(m_recommendations, &LibraryRecommendations::changed, this, &HomeTab::updateAi);
	connect(m_recommendations, &LibraryRecommendations::progress, this, [this](int done, int total) {
		m_aiProgress->setRange(0, std::max(1, total));
		m_aiProgress->setValue(done);
		updateAi();
	});
	connect(m_recommendations, &LibraryRecommendations::indexFinished, this, [this]() {
		if (m_profile->getSettings()->value("Discover/visual", false).toBool()) {
			m_feed->setVisualEnabled(true);
			updateHeader();
		}
		updateAi();
	});

	m_feed->setScope(settings->value("Home/collection", 0).toLongLong());
	if (!settings->contains("Discover/visual")) {
		// Whoever already set up the local model gets smart matching without asking again.
		settings->setValue("Discover/visual", settings->value("recommendations/localEnabled", false).toBool());
	}
	m_feed->setVisualEnabled(settings->value("Discover/visual", false).toBool());
	updateScopes();
	updateHeader();
	updateChips();
	updateAi();
}

HomeTab::~HomeTab()
{
	m_feed->cancel();
}

ImageGridView *HomeTab::grid() const { return m_grid; }
DiscoveryFeed *HomeTab::feed() const { return m_feed; }

void HomeTab::showEvent(QShowEvent *event)
{
	QWidget::showEvent(event);
	m_grid->setDensity(m_profile->getSettings()->value("Gallery/density", 1).toInt());
	if (!m_started) {
		m_started = true;
		refresh();
	}
}

void HomeTab::refresh()
{
	m_started = true;
	m_grid->gridModel()->clear();
	m_libraryKeys.clear();
	m_status->clear();
	m_feed->restart();
	updateHeader();
	updateChips();
	m_empty->setText(tr("Finding pictures you'll like…"));
	m_stack->setCurrentWidget(m_empty);
	m_feed->fetchMore();
}

void HomeTab::updateScopes()
{
	const QSignalBlocker blocker(m_scope);
	const qint64 current = m_feed->scope();
	m_scope->clear();
	m_scope->addItem(tr("Everything I love"), 0);
	for (const auto &collection : m_store->collections()) {
		m_scope->addItem(tr("Like %1").arg(collection.name), collection.id);
	}
	const int index = m_scope->findData(current);
	m_scope->setCurrentIndex(index < 0 ? 0 : index);
	if (index < 0 && current != 0) {
		m_feed->setScope(0);
	}
	m_scope->setVisible(m_scope->count() > 1);
}

void HomeTab::updateHeader()
{
	const auto &taste = m_feed->taste();
	if (taste.isEmpty()) {
		m_subtitle->setText(tr("Fresh pictures from your sources. Like ♥ or favorite ★ what you enjoy and Discover learns your taste."));
		return;
	}
	QString text = tr("Tuned to %n favorite(s)", "", taste.favorites()) + tr(" and %n like(s)", "", taste.likes());
	if (m_feed->visualActive()) {
		text += tr(" · smart visual matching on");
	}
	m_subtitle->setText(text + tr(" · favorites count 3× more"));
}

void HomeTab::updateChips()
{
	while (auto *item = m_chipsLayout->takeAt(0)) {
		delete item->widget();
		delete item;
	}
	const auto &taste = m_feed->taste();
	QList<TasteTag> tags = taste.topTags(4, {"artist"}) + taste.topTags(4, {"character"}) + taste.topTags(3, {"copyright"});
	if (tags.size() < 6) {
		for (const auto &tag : taste.topTags(12)) {
			bool known = false;
			for (const auto &existing : tags) {
				known = known || existing.name == tag.name;
			}
			if (!known && tags.size() < 8) {
				tags.append(tag);
			}
		}
	}
	if (tags.isEmpty()) {
		m_chips->hide();
		return;
	}
	auto *label = new QLabel(tr("Your top picks:"), m_chips);
	label->setObjectName("pageSubtitle");
	m_chipsLayout->addWidget(label);
	for (const auto &tag : tags.mid(0, 10)) {
		const QString prefix = tag.type == QLatin1String("artist") ? QStringLiteral("🎨 ") : tag.type == QLatin1String("character") ? QStringLiteral("👤 ") : QStringLiteral("# ");
		auto *chip = new QPushButton(prefix + QString(tag.name).replace('_', ' '), m_chips);
		chip->setCursor(Qt::PointingHandCursor);
		chip->setToolTip(tr("Search %1 in a new tab").arg(tag.name));
		connect(chip, &QPushButton::clicked, this, [this, name = tag.name]() {
			emit searchRequested(name);
			if (m_mainWindow != nullptr) {
				m_mainWindow->loadTag(name, true, false);
			}
		});
		m_chipsLayout->addWidget(chip);
	}
	m_chipsLayout->addStretch();
	m_chips->show();
}

void HomeTab::updateAi()
{
	QSettings *settings = m_profile->getSettings();
	const bool wanted = settings->value("Discover/visual", false).toBool();
	const bool busy = m_recommendations->busy();
	const bool hasTaste = !m_feed->taste().isEmpty();
	m_aiProgress->setVisible(busy);
	if (busy) {
		m_aiText->setText(tr("<b>Setting up smart visual matching…</b> %1").arg(m_recommendations->status().toHtmlEscaped()));
		m_aiEnable->hide();
		m_aiBanner->show();
		return;
	}
	m_aiEnable->show();
	const bool dismissed = settings->value("Discover/aiBannerDismissed", false).toBool();
	m_aiText->setText(tr("<b>Smarter picks with local AI.</b> Finds pictures that look like your favorites, even when tags differ. "
		"Runs entirely on this PC; nothing is uploaded. One-time 89 MB download."));
	m_aiBanner->setVisible(!wanted && !dismissed && hasTaste);
}

void HomeTab::enableAi()
{
	QSettings *settings = m_profile->getSettings();
	settings->setValue("Discover/visual", true);
	settings->setValue("recommendations/localEnabled", true);
	if (!m_recommendations->modelAvailable()) {
		m_recommendations->downloadModel();
	} else if (m_recommendations->indexedCount() == 0) {
		m_recommendations->startIndexing();
	}
	m_feed->setVisualEnabled(true);
	Toast::show(this, m_recommendations->modelAvailable() ? tr("Smart visual matching is on.") : tr("Downloading the visual model… Discover keeps working meanwhile."));
	updateAi();
	updateHeader();
}

void HomeTab::addItems(const QList<DiscoveryItem> &items)
{
	QList<ImageGridItem> tiles;
	const qint64 now = QDateTime::currentMSecsSinceEpoch();
	for (const auto &item : items) {
		ImageGridItem tile;
		tile.key = item.key;
		tile.pixmap = item.image->previewImage();
		tile.shownAt = now;
		tile.title = pictureTitle(item.image);
		QStringList details {tile.title, item.reason};
		if (item.visual) {
			details.append(tr("Looks like pictures you love"));
		}
		const QSize size = item.image->size();
		if (size.isValid() && !size.isEmpty()) {
			details.append(QStringLiteral("%1 × %2").arg(size.width()).arg(size.height()));
		}
		tile.tooltip = details.join('\n');
		tile.badge = item.image->isVideo() ? QStringLiteral("▶") : (!item.image->isAnimated().isEmpty() ? QStringLiteral("GIF") : QString());
		tiles.append(tile);
	}
	m_grid->gridModel()->append(tiles);
	m_grid->startFade();
	if (m_grid->gridModel()->rowCount() > 0) {
		m_stack->setCurrentWidget(m_grid);
	}
	updateHeader();
}

QString HomeTab::libraryKey(const QString &key)
{
	if (!m_libraryKeys.contains(key)) {
		if (const auto image = m_feed->image(key)) {
			m_libraryKeys.insert(key, m_store->keyForImage(*image));
		}
	}
	return m_libraryKeys.value(key);
}

void HomeTab::rate(const QString &key, bool favorite)
{
	const auto image = m_feed->image(key);
	if (!image) {
		return;
	}
	const QString saved = m_store->saveImage(*image);
	if (saved.isEmpty()) {
		Toast::show(this, m_store->lastError(), 3500);
		return;
	}
	m_libraryKeys.insert(key, saved);
	const qint64 scope = m_feed->scope();
	bool ok = false;
	const bool on = PictureActions::toggleRating(m_store, saved, scope, favorite, &ok);
	if (!ok) {
		Toast::show(this, m_store->lastError(), 3500);
		return;
	}
	if (!on && scope == 0) {
		// Un-rating a picture that only entered the Library through Discover leaves no trace.
		const auto entry = m_store->entry(saved);
		if (!entry.liked && !entry.favorite && entry.collectionCount == 0 && entry.notes.isEmpty() && entry.localPaths.isEmpty()) {
			m_store->removeImage(saved);
		}
	}
	const auto entry = m_store->entry(saved, scope);
	m_grid->gridModel()->setRating(key, entry.liked, entry.favorite);
	Toast::show(this, !on ? tr("Rating removed") : favorite ? tr("★ Favorited — expect more like this") : tr("♥ Liked — Discover is learning your taste"));
}

void HomeTab::syncRating(const QString &changed)
{
	for (auto it = m_libraryKeys.constBegin(); it != m_libraryKeys.constEnd(); ++it) {
		if (changed.isEmpty() || it.value() == changed) {
			const auto entry = m_store->entry(it.value(), m_feed->scope());
			m_grid->gridModel()->setRating(it.key(), entry.liked, entry.favorite);
		}
	}
}

void HomeTab::triggerAction(ImageGridView::Action action, const QStringList &keys)
{
	switch (action) {
		case ImageGridView::Like:
		case ImageGridView::Favorite:
			for (const auto &key : keys) {
				rate(key, action == ImageGridView::Favorite);
			}
			break;
		case ImageGridView::Download: {
			QList<QSharedPointer<Image>> images;
			for (const auto &key : keys) {
				if (const auto image = m_feed->image(key)) {
					images.append(image);
				}
			}
			PictureActions::download(m_profile, m_downloadQueue, images, this);
			break;
		}
		case ImageGridView::Hide:
			for (const auto &key : keys) {
				m_feed->dismiss(key);
				m_grid->gridModel()->remove(key);
			}
			Toast::show(this, tr("Hidden. You'll see fewer pictures like this."));
			break;
	}
}

void HomeTab::openPicture(const QString &key)
{
	const auto image = m_feed->image(key);
	if (!image) {
		return;
	}
	QList<QSharedPointer<Image>> images;
	for (const auto &other : m_grid->gridModel()->keys()) {
		if (const auto candidate = m_feed->image(other)) {
			images.append(candidate);
		}
	}
	const qint64 scope = m_feed->scope();
	auto *viewer = new ViewerWindow(images, image, image->parentSite(), m_profile, m_mainWindow, nullptr, scope > 0 && m_store->contains(libraryKey(key), scope) ? scope : 0);
	viewer->show();
}

void HomeTab::showMenu(const QStringList &keys, const QPoint &position)
{
	if (keys.isEmpty()) {
		return;
	}
	QMenu menu(this);
	const QString key = keys.first();
	const auto image = m_feed->image(key);
	if (keys.size() == 1) {
		menu.addAction(tr("Open"), this, [this, key]() { openPicture(key); });
	}
	menu.addAction(tr("♥ Like"), this, [this, keys]() { triggerAction(ImageGridView::Like, keys); });
	menu.addAction(tr("★ Favorite"), this, [this, keys]() { triggerAction(ImageGridView::Favorite, keys); });
	auto *collections = menu.addMenu(tr("Add to collection"));
	for (const auto &collection : m_store->collections()) {
		collections->addAction(collection.name, this, [this, keys, id = collection.id, name = collection.name]() {
			for (const auto &selected : keys) {
				if (const auto picture = m_feed->image(selected)) {
					const QString saved = m_store->saveImage(*picture);
					if (saved.isEmpty() || !m_store->addToCollection(saved, id)) {
						Toast::show(this, m_store->lastError(), 3500);
						return;
					}
					m_libraryKeys.insert(selected, saved);
				}
			}
			Toast::show(this, tr("Added to %1").arg(name));
		});
	}
	collections->setEnabled(!collections->isEmpty());
	menu.addAction(tr("Download"), this, [this, keys]() { triggerAction(ImageGridView::Download, keys); });
	if (keys.size() == 1 && image) {
		menu.addSeparator();
		QStringList best;
		const auto &taste = m_feed->taste();
		QStringList tags = image->tagsString();
		std::sort(tags.begin(), tags.end(), [&taste](const QString &left, const QString &right) { return taste.weight(left) > taste.weight(right); });
		for (const auto &tag : tags) {
			if (!TasteProfile::isMetaTag(TasteProfile::normalize(tag)) && !tag.contains(':') && best.size() < 2) {
				best.append(tag);
			}
		}
		if (!best.isEmpty()) {
			menu.addAction(tr("More like this: %1").arg(best.join(' ')), this, [this, best]() {
				emit searchRequested(best.join(' '));
				if (m_mainWindow != nullptr) {
					m_mainWindow->loadTag(best.join(' '), true, false);
				}
			});
		}
		const QUrl page = image->pageUrl();
		if (page.isValid() && (page.scheme() == "http" || page.scheme() == "https")) {
			menu.addAction(tr("Open source page"), this, [page]() { QDesktopServices::openUrl(page); });
			menu.addAction(tr("Copy link"), this, [page]() { QApplication::clipboard()->setText(page.toString()); });
		}
	}
	menu.addSeparator();
	menu.addAction(tr("Not interested"), this, [this, keys]() { triggerAction(ImageGridView::Hide, keys); });
	menu.exec(position);
}
