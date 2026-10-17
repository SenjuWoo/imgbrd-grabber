#include <QApplication>
#include <QFile>
#include <QImage>
#include <QItemSelectionModel>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPainter>
#include <QTableWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QScopedPointer>
#include <QScopeGuard>
#include <QSettings>
#include <QSignalSpy>
#include <QTabBar>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTest>
#include "catch.h"
#include "custom-network-access-manager.h"
#include "discover-dialogs.h"
#include "discovery-feed.h"
#include "models/image.h"
#include "models/library-store.h"
#include "models/profile.h"
#include "models/site.h"
#include "source-helpers.h"
#include "tabs/home-tab.h"
#include "tabs/search-tab.h"
#include "tabs/tabs-loader.h"
#include "tabs/tag-tab.h"
#include "tags/tag.h"
#include "ui/image-grid.h"
#include "ui/toast.h"
#include "viewer/viewer-window.h"

namespace
{
	QString saveRated(Profile *profile, int id, const QString &md5, const QString &tags, bool favorite)
	{
		Image image(profile->getSites().value("danbooru.donmai.us"), {{"id", QString::number(id)}, {"md5", md5},
			{"file_url", "https://test.invalid/" + QString::number(id) + ".png"}, {"tags", tags}}, profile);
		QList<Tag> imageTags;
		for (const auto &tag : tags.split(' ', Qt::SkipEmptyParts)) {
			imageTags.append(Tag(tag, "general"));
		}
		image.setTags(imageTags);
		QPixmap preview(64, 80);
		preview.fill(Qt::darkGreen);
		image.setPreviewImage(preview);
		const QString key = profile->library()->saveImage(image);
		REQUIRE(!key.isEmpty());
		REQUIRE((favorite ? profile->library()->setFavorite(key, true) : profile->library()->setLiked(key, true)));
		return key;
	}

	Site *jsonDanbooru(Profile *profile)
	{
		auto *site = profile->getSites().value("danbooru.donmai.us");
		REQUIRE(site != nullptr);
		site->setSetting("sources/usedefault", false, true);
		site->setSetting("sources/source_1", "Json", "Xml");
		site->setSetting("sources/source_2", "", "Json");
		site->setSetting("sources/source_3", "", "Regex");
		site->setSetting("sources/source_4", "", "Rss");
		site->loadConfig();
		profile->getSettings()->setValue("sites", QStringList{site->url()});
		return site;
	}

	QString writePreview(const QTemporaryDir &directory, const QString &name, const QColor &color)
	{
		QImage image(48, 60, QImage::Format_RGB32);
		image.fill(color);
		const QString path = directory.filePath(name);
		REQUIRE(image.save(path));
		return path;
	}

	QString writePosts(const QTemporaryDir &directory, const QString &name, const QJsonArray &posts)
	{
		QFile file(directory.filePath(name));
		REQUIRE(file.open(QIODevice::WriteOnly));
		REQUIRE(file.write(QJsonDocument(posts).toJson()) > 0);
		return file.fileName();
	}

	QJsonObject post(int id, const QString &md5, const QString &tags)
	{
		return {{"id", id}, {"md5", md5}, {"file_url", QStringLiteral("https://test.invalid/%1.png").arg(id)}, {"tag_string", tags}, {"image_width", 800}, {"image_height", 1000}};
	}

	// A well-sampled source, so Discover searches right away instead of sampling newest posts first.
	void seedTagBackground(Profile *profile, const QString &website)
	{
		const QJsonObject background {{website, QJsonObject {{"posts", 400}, {"tags", QJsonObject {{"forest", 4}}}}}};
		QFile file(DiscoveryFeed::statePath(profile->getPath()));
		REQUIRE(file.open(QIODevice::WriteOnly));
		REQUIRE(file.write(QJsonDocument(QJsonObject {{"version", 2}, {"background", background}}).toJson()) > 0);
	}
}

TEST_CASE("Discover learns from ratings and hides rated, blacklisted and dismissed pictures", "[library][home][discovery]")
{
	auto clearReplies = qScopeGuard([]() { CustomNetworkAccessManager::NextFiles.clear(); });
	QTemporaryDir directory;
	REQUIRE(directory.isValid());
	const QScopedPointer<Profile> profile(makeLibraryProfile(directory.path()));
	Site *site = jsonDanbooru(profile.data());
	seedTagBackground(profile.data(), site->url());
	const QString rated = saveRated(profile.data(), 801, "22222222222222222222222222222222", "forest", true);
	profile->addBlacklistedTag("spiders");
	const QString posts = writePosts(directory, "posts.json", {
		post(901, "11111111111111111111111111111111", "forest river"),
		post(801, "22222222222222222222222222222222", "forest"), // Already favorited.
		post(903, "33333333333333333333333333333333", "forest spiders"), // Blacklisted.
		post(904, "44444444444444444444444444444444", "forest lake"),
	});
	CustomNetworkAccessManager::NextFiles.enqueue(posts);
	CustomNetworkAccessManager::NextFiles.enqueue(writePreview(directory, "a.png", Qt::red));
	CustomNetworkAccessManager::NextFiles.enqueue(writePreview(directory, "b.png", Qt::blue));

	HomeTab home(profile.data(), nullptr);
	home.resize(1100, 800);
	home.show();
	auto *grid = home.grid();
	auto *model = grid->gridModel();
	REQUIRE(QTest::qWaitFor([model]() { return model->rowCount() == 2; }, 8000));
	QStringList ids;
	for (const auto &key : model->keys()) {
		ids.append(QString::number(home.feed()->image(key)->id()));
		REQUIRE(model->item(model->row(key)).tooltip.contains("Because you like forest"));
	}
	ids.sort();
	REQUIRE(ids == QStringList {"901", "904"});
	REQUIRE(home.findChild<QLabel*>("pageSubtitle")->text().contains("1 favorite"));

	// Keyboard: L likes, L again removes it again, F favorites. Like and Favorite stay exclusive.
	const QString first = model->item(0).key;
	grid->setFocus();
	grid->selectionModel()->select(model->index(0), QItemSelectionModel::ClearAndSelect);
	grid->setCurrentIndex(model->index(0));
	QTest::keyClick(grid, Qt::Key_L);
	const QString libraryKey = profile->library()->keyForImage(*home.feed()->image(first));
	REQUIRE(profile->library()->entry(libraryKey).liked);
	// The search that found it gets credit once, for later batches.
	REQUIRE(home.feed()->feedback().value("forest").shown == 2);
	REQUIRE(home.feed()->feedback().value("forest").rated == 1);
	REQUIRE(model->item(0).liked);
	REQUIRE(profile->library()->entry(libraryKey).image.value("website").toString() == site->url());
	QTest::keyClick(grid, Qt::Key_L);
	// Un-rating asks whether to remove the picture from Library too.
	QMessageBox *ask = nullptr;
	REQUIRE(QTest::qWaitFor([&ask]() {
		for (auto *widget : QApplication::topLevelWidgets()) {
			auto *box = qobject_cast<QMessageBox*>(widget);
			ask = box != nullptr && box->isVisible() ? box : ask;
		}
		return ask != nullptr;
	}, 3000));
	REQUIRE(ask->text().contains("no longer liked"));
	for (auto *button : ask->buttons()) {
		if (button->text() == "Remove") {
			button->click();
		}
	}
	REQUIRE(QTest::qWaitFor([&]() { return !profile->library()->contains(libraryKey); }, 3000));
	QTest::keyClick(grid, Qt::Key_F);
	REQUIRE(profile->library()->entry(libraryKey).favorite);
	REQUIRE_FALSE(profile->library()->entry(libraryKey).liked);
	REQUIRE(model->item(0).favorite);

	// Hide: gone from the feed and remembered across sessions.
	const QString second = model->item(1).key;
	grid->selectionModel()->select(model->index(1), QItemSelectionModel::ClearAndSelect);
	QTest::keyClick(grid, Qt::Key_X);
	REQUIRE(model->rowCount() == 1);
	REQUIRE(home.feed()->dismissedCount() == 1);
	QFile state(DiscoveryFeed::statePath(profile->getPath()));
	REQUIRE(state.open(QIODevice::ReadOnly));
	const auto saved = QJsonDocument::fromJson(state.readAll()).object();
	REQUIRE(saved.value("hidden").toArray().contains(second));
	// Shown pictures count as seen only once the next batch is fetched, so this one is not marked seen yet.
	REQUIRE_FALSE(home.feed()->isSeen(second));
	REQUIRE(profile->library()->entry(rated).favorite);
	REQUIRE(home.feed()->feedback().value("forest").rated == 1);
	REQUIRE(home.feed()->feedback().value("forest").shown == 4); // Hiding counts more than scrolling past.
	REQUIRE(saved.value("feedback").toObject().value("forest").toArray().at(0).toDouble() == 4);

	// Double-clicking a picture opens it in a visible viewer.
	const QRect tile(grid->visualRect(model->index(0)).topLeft(), grid->tileSize());
	QTest::mouseDClick(grid->viewport(), Qt::LeftButton, Qt::NoModifier, tile.center() - QPoint(0, tile.height() / 4));
	QWidget *viewer = nullptr;
	REQUIRE(QTest::qWaitFor([&viewer]() {
		for (auto *widget : QApplication::topLevelWidgets()) {
			if (qobject_cast<ViewerWindow*>(widget) != nullptr && widget->isVisible()) {
				viewer = widget;
			}
		}
		return viewer != nullptr;
	}, 3000));
	viewer->close();
}

TEST_CASE("Discover samples newest posts to learn how common tags are on each source", "[library][home][discovery]")
{
	auto clearReplies = qScopeGuard([]() { CustomNetworkAccessManager::NextFiles.clear(); });
	QTemporaryDir directory;
	REQUIRE(directory.isValid());
	const QScopedPointer<Profile> profile(makeLibraryProfile(directory.path()));
	Site *site = jsonDanbooru(profile.data());
	// All three searches go out at once, so the mocked replies are taken in order.
	site->setSetting("download/throttle_page", 0, 1);
	site->loadConfig();
	saveRated(profile.data(), 801, "22222222222222222222222222222222", "forest", true);
	profile->addBlacklistedTag("spiders");
	const QString posts = writePosts(directory, "posts.json", {
		post(901, "11111111111111111111111111111111", "forest river"),
		post(801, "22222222222222222222222222222222", "forest"),
		post(903, "33333333333333333333333333333333", "forest spiders"),
		post(904, "44444444444444444444444444444444", "forest lake"),
	});
	// One newest-post sample (one source still being learned) and the "forest" search, in any order, then two previews.
	for (int i = 0; i < 2; ++i) {
		CustomNetworkAccessManager::NextFiles.enqueue(posts);
	}
	CustomNetworkAccessManager::NextFiles.enqueue(writePreview(directory, "a.png", Qt::red));
	CustomNetworkAccessManager::NextFiles.enqueue(writePreview(directory, "b.png", Qt::blue));

	HomeTab home(profile.data(), nullptr);
	home.resize(1100, 800);
	home.show();
	auto *model = home.grid()->gridModel();
	REQUIRE(QTest::qWaitFor([&home]() { return !home.feed()->isBusy() && home.grid()->gridModel()->rowCount() == 2; }, 8000));
	REQUIRE(model->rowCount() == 2); // Samples still never show rated, blacklisted or repeated pictures.
	// Every valid sampled post counts, rated and blacklisted ones too: they are part of what the source has.
	REQUIRE(home.feed()->background().posts(site->url()) == 4);
	QFile state(DiscoveryFeed::statePath(profile->getPath()));
	REQUIRE(state.open(QIODevice::ReadOnly));
	const auto saved = QJsonDocument::fromJson(state.readAll()).object();
	REQUIRE(TagBackground::fromJson(saved.value("background").toObject()).frequency("forest") < 0); // Not yet well sampled.
	REQUIRE(saved.value("background").toObject().value(site->url()).toObject().value("posts").toDouble() == 4);
}

TEST_CASE("Discover marks shown pictures seen only once the next batch is fetched", "[library][home][discovery]")
{
	auto clearReplies = qScopeGuard([]() { CustomNetworkAccessManager::NextFiles.clear(); });
	QTemporaryDir directory;
	REQUIRE(directory.isValid());
	const QScopedPointer<Profile> profile(makeLibraryProfile(directory.path()));
	Site *site = jsonDanbooru(profile.data());
	seedTagBackground(profile.data(), site->url());
	saveRated(profile.data(), 801, QString(32, '8'), "forest", true);
	CustomNetworkAccessManager::NextFiles.enqueue(writePosts(directory, "posts.json", {
		post(901, QString(32, '1'), "forest river"),
		post(902, QString(32, '2'), "forest lake"),
	}));
	CustomNetworkAccessManager::NextFiles.enqueue(writePreview(directory, "a.png", Qt::red));
	CustomNetworkAccessManager::NextFiles.enqueue(writePreview(directory, "b.png", Qt::blue));

	HomeTab home(profile.data(), nullptr);
	home.resize(1100, 800);
	home.show();
	auto *feed = home.feed();
	REQUIRE(QTest::qWaitFor([&home]() { return !home.feed()->isBusy() && home.grid()->gridModel()->rowCount() == 2; }, 8000));
	const QString first = home.grid()->gridModel()->item(0).key;
	const QString second = home.grid()->gridModel()->item(1).key;
	// Loaded and on screen, but not asked past: still in the repeat window.
	REQUIRE_FALSE(feed->isSeen(first));
	REQUIRE_FALSE(feed->isSeen(second));

	feed->fetchMore(); // Asking for the next batch is what marks the shown one seen.
	REQUIRE(feed->isSeen(first));
	REQUIRE(feed->isSeen(second));
}

TEST_CASE("Discover reports failed sources and starts fresh without ratings", "[library][home][discovery]")
{
	auto clearReplies = qScopeGuard([]() { CustomNetworkAccessManager::NextFiles.clear(); });
	QTemporaryDir directory;
	REQUIRE(directory.isValid());
	const QScopedPointer<Profile> profile(makeLibraryProfile(directory.path()));
	jsonDanbooru(profile.data());
	for (int i = 0; i < 6; ++i) {
		CustomNetworkAccessManager::NextFiles.enqueue("500");
	}
	HomeTab home(profile.data(), nullptr);
	home.resize(900, 700);
	home.show();
	REQUIRE(home.findChild<QLabel*>("pageSubtitle")->text().contains("Fresh pictures"));
	auto *status = home.findChild<QLabel*>("homeStatus");
	REQUIRE(QTest::qWaitFor([status]() { return status->text().contains("did not respond"); }, 8000));
	REQUIRE(home.grid()->gridModel()->rowCount() == 0);
	REQUIRE_FALSE(home.findChild<QFrame*>("homeAiBanner")->isVisible()); // Nothing to compare with yet.
}

TEST_CASE("Picture grid shows actions only on selected tiles and fills the width", "[library][grid]")
{
	ImageGridView grid;
	grid.resize(900, 600);
	grid.setDensity(1);
	QList<ImageGridItem> items;
	for (int i = 0; i < 30; ++i) {
		QPixmap pixmap(60, 90);
		pixmap.fill(QColor::fromHsv(i * 12, 200, 200));
		items.append({QString::number(i), pixmap, {}, QString("Picture %1").arg(i), {}, {}, i == 0, i == 1, 0});
	}
	grid.gridModel()->setItems(items);
	grid.show();
	QApplication::processEvents();
	const int columns = grid.viewport()->width() / grid.gridSize().width();
	REQUIRE(columns >= 3);
	REQUIRE(grid.viewport()->width() - columns * grid.gridSize().width() < grid.gridSize().width());
	REQUIRE(grid.tileSize().height() > grid.tileSize().width()); // 4:5 portrait tiles.

	const QModelIndex index = grid.gridModel()->index(2);
	const QRect tile(grid.visualRect(index).topLeft(), grid.tileSize());
	const QPoint likeButton = grid.actionRect(tile, 0).center();
	REQUIRE(grid.actionAt(index, likeButton) == -1); // Clean tiles until selected.
	QSignalSpy actions(&grid, &ImageGridView::actionTriggered);
	QSignalSpy opened(&grid, &ImageGridView::openRequested);
	QTest::mouseClick(grid.viewport(), Qt::LeftButton, Qt::NoModifier, tile.center());
	REQUIRE(grid.selectedKeys() == QStringList {"2"});
	REQUIRE(grid.actionAt(index, likeButton) == 0);
	QTest::mouseClick(grid.viewport(), Qt::LeftButton, Qt::NoModifier, likeButton);
	REQUIRE(actions.count() == 1);
	REQUIRE(actions.first().at(0).value<ImageGridView::Action>() == ImageGridView::Like);
	REQUIRE(actions.first().at(1).toStringList() == QStringList {"2"});
	REQUIRE(grid.selectedKeys() == QStringList {"2"}); // Action clicks keep the selection.
	QTest::keyClick(&grid, Qt::Key_Return);
	REQUIRE(opened.count() == 1);
	QTest::keyClick(&grid, Qt::Key_D);
	REQUIRE(actions.last().at(0).value<ImageGridView::Action>() == ImageGridView::Download);

	grid.gridModel()->setRating("2", false, true);
	REQUIRE(grid.gridModel()->item(2).favorite);
	grid.gridModel()->remove("2");
	REQUIRE(grid.gridModel()->rowCount() == 29);
	REQUIRE(grid.gridModel()->row("3") == 2);
	grid.gridModel()->append({{QString("3"), {}, {}, {}, {}, {}, false, false, 0}});
	REQUIRE(grid.gridModel()->rowCount() == 29); // Duplicate keys are ignored.
}

TEST_CASE("Home and movable search tabs retain named and numeric session identities", "[library][home][session]")
{
	QTemporaryDir directory;
	REQUIRE(directory.isValid());
	const QScopedPointer<Profile> profile(makeLibraryProfile(directory.path()));
	profile->getSettings()->setValue("preloadAllTabs", false);
	QTabWidget tabs;
	tabs.setMovable(true);
	auto *first = new TagTab(profile.data(), nullptr, nullptr);
	auto *second = new TagTab(profile.data(), nullptr, nullptr);
	first->setTags("forest", false);
	second->setTags("city", false);
	auto *home = new HomeTab(profile.data(), nullptr);
	tabs.addTab(first, "Forest");
	tabs.addTab(second, "City");
	tabs.addTab(home, "Home");
	tabs.tabBar()->moveTab(2, 0);
	QList<SearchTab*> searches {first, second};
	const QString path = directory.filePath("tabs.json");
	REQUIRE(TabsLoader::save(path, searches, home));
	QList<SearchTab*> restored;
	QVariant current;
	REQUIRE(TabsLoader::load(path, restored, current, profile.data(), nullptr, nullptr));
	REQUIRE(current.toString() == "home");
	REQUIRE(restored.size() == 2);
	REQUIRE(restored.first()->tags() == "forest");
	REQUIRE(restored.last()->tags() == "city");
	qDeleteAll(restored);
	restored.clear();
	REQUIRE(TabsLoader::save(path, searches, second));
	REQUIRE(TabsLoader::load(path, restored, current, profile.data(), nullptr, nullptr));
	REQUIRE(current.toInt() == 1);
	REQUIRE(tabs.indexOf(second) != current.toInt());
	REQUIRE(restored.value(current.toInt())->tags() == "city");
	qDeleteAll(restored);
}

namespace
{
	QImage paintedArt(QColor accent, const QSize &size = QSize(240, 300))
	{
		QImage art(size, QImage::Format_RGB32);
		art.fill(QColor(30, 40, 90));
		QPainter painter(&art);
		for (int i = 0; i < 10; ++i) {
			painter.fillRect(QRectF(size.width() * (i % 5) / 5.0, size.height() * (i / 5) / 2.0 + i * 3, size.width() / 7.0, size.height() / 5.0), QColor::fromHsv((i * 37 + accent.hue()) % 360, 220, 230));
		}
		return art;
	}

	QString writeArt(const QTemporaryDir &directory, const QString &name, const QImage &art)
	{
		const QString path = directory.filePath(name);
		REQUIRE(art.save(path));
		return path;
	}
}

TEST_CASE("Discover never shows Library pictures, even copies from other sources", "[library][home][discovery]")
{
	auto clearReplies = qScopeGuard([]() { CustomNetworkAccessManager::NextFiles.clear(); });
	QTemporaryDir directory;
	REQUIRE(directory.isValid());
	const QScopedPointer<Profile> profile(makeLibraryProfile(directory.path()));
	Site *site = jsonDanbooru(profile.data());
	seedTagBackground(profile.data(), site->url());
	// Liked earlier from another post: same picture, different file.
	const QImage liked = paintedArt(Qt::red);
	Image saved(site, {{"id", "700"}, {"md5", QString(32, '7')}, {"file_url", "https://test.invalid/700.png"}, {"tags", "forest"}}, profile.data());
	saved.setTags({Tag("forest", "general")});
	saved.setPreviewImage(QPixmap::fromImage(liked));
	const QString key = profile->library()->saveImage(saved);
	REQUIRE(profile->library()->setLiked(key, true));
	CustomNetworkAccessManager::NextFiles.enqueue(writePosts(directory, "posts.json", {
		post(901, QString(32, '1'), "forest river"),
		post(902, QString(32, '2'), "forest lake"),
		post(903, QString(32, '3'), "forest edit"),
	}));
	CustomNetworkAccessManager::NextFiles.enqueue(writeArt(directory, "copy.jpg", liked.scaled(120, 150, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)));
	CustomNetworkAccessManager::NextFiles.enqueue(writeArt(directory, "other.png", paintedArt(Qt::blue).mirrored(true, false)));
	// An edit of the liked picture (a caption added) is another picture: it shows up.
	QImage edit = liked;
	QPainter painter(&edit);
	painter.fillRect(QRect(20, 20, 140, 120), Qt::black);
	painter.end();
	CustomNetworkAccessManager::NextFiles.enqueue(writeArt(directory, "edit.png", edit));

	HomeTab home(profile.data(), nullptr);
	home.resize(1100, 800);
	home.show();
	auto *model = home.grid()->gridModel();
	REQUIRE(QTest::qWaitFor([&home, model]() { return !home.feed()->isBusy() && model->rowCount() > 0; }, 8000));
	REQUIRE(model->rowCount() == 2);
	QList<qulonglong> shown;
	for (int row = 0; row < model->rowCount(); ++row) {
		shown.append(home.feed()->image(model->item(row).key)->id());
	}
	REQUIRE(shown.contains(902));
	REQUIRE(shown.contains(903));
	for (int row = 0; row < model->rowCount(); ++row) {
		if (home.feed()->image(model->item(row).key)->id() == 902) {
			REQUIRE(model->item(row).tooltip.contains("Matches your taste: forest"));
		}
	}
}

TEST_CASE("Discover lets users say what is not interesting, undo it and restore hidden pictures", "[library][home][discovery]")
{
	auto clearReplies = qScopeGuard([]() { CustomNetworkAccessManager::NextFiles.clear(); });
	QTemporaryDir directory;
	REQUIRE(directory.isValid());
	const QScopedPointer<Profile> profile(makeLibraryProfile(directory.path()));
	Site *site = jsonDanbooru(profile.data());
	seedTagBackground(profile.data(), site->url());
	saveRated(profile.data(), 801, QString(32, '8'), "forest", true);
	CustomNetworkAccessManager::NextFiles.enqueue(writePosts(directory, "posts.json", {
		post(901, QString(32, '1'), "forest river"),
		post(902, QString(32, '2'), "forest lake"),
	}));
	CustomNetworkAccessManager::NextFiles.enqueue(writePreview(directory, "a.png", Qt::red));
	CustomNetworkAccessManager::NextFiles.enqueue(writePreview(directory, "b.png", Qt::blue));

	HomeTab home(profile.data(), nullptr);
	home.resize(1100, 800);
	home.show();
	auto *grid = home.grid();
	auto *model = grid->gridModel();
	REQUIRE(QTest::qWaitFor([&home, model]() { return !home.feed()->isBusy() && model->rowCount() == 2; }, 8000));
	auto *feed = home.feed();

	// X hides at once; Undo in the toast brings the picture back where it was.
	const QString first = model->item(0).key;
	grid->setFocus();
	grid->selectionModel()->select(model->index(0), QItemSelectionModel::ClearAndSelect);
	QTest::keyClick(grid, Qt::Key_X);
	REQUIRE(model->rowCount() == 1);
	REQUIRE(feed->dismissedCount() == 1);
	auto *toast = home.findChild<Toast*>(QString(), Qt::FindDirectChildrenOnly);
	REQUIRE(toast != nullptr);
	REQUIRE(toast->text().contains("Undo"));
	emit toast->linkActivated("action");
	REQUIRE(model->rowCount() == 2);
	REQUIRE(model->item(0).key == first);
	REQUIRE(feed->dismissedCount() == 0);
	REQUIRE(feed->taste().weight("river") >= 0); // Its tags were not held against it.

	// Naming a tag affects only that tag, and the picture can be restored later.
	const QString lakeKey = feed->image(model->item(1).key)->id() == 902 ? model->item(1).key : model->item(0).key;
	feed->dismiss(lakeKey, {"lake"}, false);
	REQUIRE(feed->tuning().tags.value("lake") == TasteTuning::Less);
	REQUIRE(feed->taste().weight("lake") < 0);
	REQUIRE(feed->taste().weight("forest") > 0);
	REQUIRE(feed->hiddenPictures().size() == 1);
	REQUIRE(feed->hiddenPictures().first().image->id() == 902);

	CustomNetworkAccessManager::NextFiles.enqueue(writePreview(directory, "c.png", Qt::blue));
	{
		DiscoverHiddenDialog dialog(feed, &home);
		dialog.show();
		auto *list = dialog.findChild<QListWidget*>("discoverHiddenList");
		REQUIRE(list->count() == 1);
		list->item(0)->setSelected(true);
		auto *restore = dialog.findChild<QPushButton*>("discoverRestoreSelected");
		REQUIRE(restore->isEnabled());
		QTest::mouseClick(restore, Qt::LeftButton);
		REQUIRE(list->count() == 0);
		REQUIRE(dialog.restoredCount() == 1);
	}
	REQUIRE(feed->dismissedCount() == 0);
	REQUIRE(feed->hiddenPictures().isEmpty());

	// A search hides pictures Discover never showed, and Never applies to searches too.
	auto searched = QSharedPointer<Image>::create(site, QMap<QString, QString> {{"id", "950"}, {"md5", QString(32, 'e')}, {"file_url", "https://test.invalid/950.png"}, {"tags", "forest spiders"}}, profile.data());
	const QString searchedKey = LibraryStore::imageKey(*searched);
	feed->dismiss(searchedKey, {"spiders"}, true, searched);
	REQUIRE(feed->isHidden(searchedKey));
	REQUIRE(feed->neverShown({"forest", "Spiders"}) == "Spiders");
	REQUIRE(feed->hiddenPictures().first().image->id() == 950);
	feed->restore({searchedKey});
	feed->setTuning({});
	REQUIRE_FALSE(feed->isHidden(searchedKey));
	REQUIRE(feed->neverShown({"spiders"}).isEmpty());

	// Hidden pictures and tuning survive a restart.
	feed->dismiss(lakeKey, {"lake"}, false);
	DiscoveryFeed reloaded(profile.data(), nullptr);
	REQUIRE(reloaded.dismissedCount() == 1);
	REQUIRE(reloaded.hiddenPictures().size() == 1);
	REQUIRE(reloaded.tuning().tags.value("lake") == TasteTuning::Less);

	// Pictures hidden before previews were kept still get a row and can be restored one by one.
	QFile state(DiscoveryFeed::statePath(profile->getPath()));
	REQUIRE(state.open(QIODevice::ReadOnly));
	QJsonObject json = QJsonDocument::fromJson(state.readAll()).object();
	state.close();
	QJsonArray hidden = json.value("hidden").toArray();
	hidden.prepend(QString(64, 'f'));
	json.insert("hidden", hidden);
	REQUIRE(state.open(QIODevice::WriteOnly | QIODevice::Truncate));
	state.write(QJsonDocument(json).toJson());
	state.close();
	DiscoveryFeed legacy(profile.data(), nullptr);
	REQUIRE(legacy.legacyHidden() == QStringList {QString(64, 'f')});
	CustomNetworkAccessManager::NextFiles.enqueue(writePreview(directory, "d.png", Qt::blue));
	DiscoverHiddenDialog dialog(&legacy, &home);
	auto *list = dialog.findChild<QListWidget*>("discoverHiddenList");
	REQUIRE(list->count() == 2);
	REQUIRE(list->item(1)->text().contains("No preview kept"));
	list->item(1)->setSelected(true);
	QTest::mouseClick(dialog.findChild<QPushButton*>("discoverRestoreSelected"), Qt::LeftButton);
	REQUIRE(legacy.legacyHidden().isEmpty());
	REQUIRE(legacy.dismissedCount() == 1);
}

TEST_CASE("The taste dialog explains Discover and edits focus, variety and tag preferences", "[library][home][discovery]")
{
	QTemporaryDir directory;
	REQUIRE(directory.isValid());
	const QScopedPointer<Profile> profile(makeLibraryProfile(directory.path()));
	Site *site = jsonDanbooru(profile.data());
	seedTagBackground(profile.data(), site->url());
	saveRated(profile.data(), 801, QString(32, '8'), "forest", true);
	saveRated(profile.data(), 802, QString(32, '9'), "forest river", false);
	DiscoveryFeed feed(profile.data(), nullptr);
	DiscoverTasteDialog dialog(&feed);
	REQUIRE(dialog.findChild<QLabel*>("discoverHow")->text().contains("How Discover picks pictures"));
	REQUIRE(dialog.findChild<QLabel*>("discoverStats")->text().contains("1 favorite"));
	auto *table = dialog.findChild<QTableWidget*>("discoverTasteTags");
	int forest = -1;
	for (int row = 0; row < table->rowCount(); ++row) {
		forest = table->item(row, 0)->text() == "forest" ? row : forest;
	}
	REQUIRE(forest >= 0);
	REQUIRE(table->item(forest, 3)->text() == "2");
	REQUIRE(dialog.findChild<QPushButton*>("discoverShowHidden")->text().contains("Hidden pictures (0)"));
	for (int row = 0; row < table->rowCount(); ++row) {
		if (table->item(row, 0)->data(Qt::UserRole).toString() == "river") {
			auto *river = static_cast<QComboBox*>(table->cellWidget(row, 5));
			river->setCurrentIndex(river->findData(int(TasteTuning::Neutral)));
		}
	}
	auto *choice = static_cast<QComboBox*>(table->cellWidget(forest, 5));
	choice->setCurrentIndex(choice->findData(int(TasteTuning::Never)));
	dialog.findChild<QLineEdit*>("discoverFocus")->setText("Umbrella, beach");
	auto *variety = dialog.findChild<QComboBox*>("discoverVariety");
	variety->setCurrentIndex(variety->findData(int(TasteTuning::Adventurous)));
	dialog.findChild<QLineEdit*>("discoverNewTag")->setText("rainy day");
	QTest::mouseClick(dialog.findChild<QPushButton*>("discoverAdd3"), Qt::LeftButton); // More
	const QString screenshot = qEnvironmentVariable("GRABBER_TASTE_SCREENSHOT");
	if (!screenshot.isEmpty()) {
		dialog.show();
		QApplication::processEvents();
		REQUIRE(dialog.grab().save(screenshot));
	}
	const auto tuning = dialog.tuning();
	REQUIRE(tuning.focus == QStringList({"umbrella", "beach"}));
	REQUIRE(tuning.variety == TasteTuning::Adventurous);
	REQUIRE(tuning.tags.value("forest") == TasteTuning::Never);
	REQUIRE(tuning.tags.value("rainy_day") == TasteTuning::More);
	REQUIRE(tuning.tags.value("river") == TasteTuning::Neutral);
	feed.setTuning(tuning);
	REQUIRE(feed.taste().blocks({"forest"}));
	REQUIRE(feed.taste().weight("river") == 0);
	REQUIRE(feed.taste().queries({site->url()}, 5, 3).first().reason.contains("focus"));

	QTest::mouseClick(dialog.findChild<QPushButton*>("discoverResetTuning"), Qt::LeftButton);
	REQUIRE(dialog.tuning().isNeutral());
}

// Developer aid: GRABBER_DIAG_PROFILE=<copy of a full profile folder> GRABBER_TASTE_SCREENSHOT=<png> gui-tests "[tastediag]".
TEST_CASE("Taste dialog screenshot for a profile copy", "[.][tastediag]")
{
	const QString directory = qEnvironmentVariable("GRABBER_DIAG_PROFILE");
	const QString screenshot = qEnvironmentVariable("GRABBER_TASTE_SCREENSHOT");
	REQUIRE((!directory.isEmpty() && !screenshot.isEmpty()));
	Profile profile(directory);
	DiscoveryFeed feed(&profile, nullptr);
	DiscoverTasteDialog dialog(&feed);
	dialog.resize(1100, 900);
	dialog.show();
	QApplication::processEvents();
	REQUIRE(dialog.grab().save(screenshot));
	auto *table = dialog.findChild<QTableWidget*>("discoverTasteTags");
	REQUIRE(table->grab().save(QString(screenshot).replace(".png", "-table.png")));
	table->scrollToBottom();
	QApplication::processEvents();
	REQUIRE(table->grab().save(QString(screenshot).replace(".png", "-bottom.png")));
}
