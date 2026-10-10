#include <QApplication>
#include <QFile>
#include <QImage>
#include <QItemSelectionModel>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
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
}

TEST_CASE("Discover learns from ratings and hides rated, blacklisted and dismissed pictures", "[library][home][discovery]")
{
	auto clearReplies = qScopeGuard([]() { CustomNetworkAccessManager::NextFiles.clear(); });
	QTemporaryDir directory;
	REQUIRE(directory.isValid());
	const QScopedPointer<Profile> profile(makeLibraryProfile(directory.path()));
	Site *site = jsonDanbooru(profile.data());
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
	REQUIRE(model->item(0).liked);
	REQUIRE(profile->library()->entry(libraryKey).image.value("website").toString() == site->url());
	QTest::keyClick(grid, Qt::Key_L);
	REQUIRE_FALSE(profile->library()->contains(libraryKey)); // Un-rating leaves no Library trace.
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
	REQUIRE(saved.value("seen").toArray().size() == 2);
	REQUIRE(profile->library()->entry(rated).favorite);
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
