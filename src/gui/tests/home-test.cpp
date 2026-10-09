#include <QApplication>
#include <QJsonArray>
#include <QImage>
#include <QComboBox>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLabel>
#include <QListWidget>
#include <QPointer>
#include <QPainter>
#include <QPainterPath>
#include <QScrollBar>
#include <QStyle>
#include <QPushButton>
#include <QScopedPointer>
#include <QScopeGuard>
#include <QSettings>
#include <QSignalSpy>
#include <QTabBar>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QToolButton>
#include "catch.h"
#include "custom-network-access-manager.h"
#include "models/site.h"
#include "image-library-actions.h"
#include "models/image.h"
#include "models/page.h"
#include "models/profile.h"
#include "source-helpers.h"
#include "tabs/home-tab.h"
#include "tabs/search-tab.h"
#include "tabs/tabs-loader.h"
#include "tabs/tag-tab.h"
#include "theme-loader.h"
#include "tags/tag.h"

namespace
{
	QPalette lightCapturePalette()
	{
		// Fusion follows Windows' dark preference; make the light capture independent of it.
		QPalette palette(QColor("#f0f0f0"));
		for (const auto group : {QPalette::Active, QPalette::Inactive, QPalette::Disabled}) {
			palette.setColor(group, QPalette::Window, QColor("#f0f0f0"));
			palette.setColor(group, QPalette::WindowText, QColor("#202020"));
			palette.setColor(group, QPalette::Base, Qt::white);
			palette.setColor(group, QPalette::AlternateBase, QColor("#f5f5f5"));
			palette.setColor(group, QPalette::Text, QColor("#202020"));
			palette.setColor(group, QPalette::Button, QColor("#eeeeee"));
			palette.setColor(group, QPalette::ButtonText, QColor("#202020"));
			palette.setColor(group, QPalette::Highlight, QColor("#0078d4"));
			palette.setColor(group, QPalette::HighlightedText, Qt::white);
			palette.setColor(group, QPalette::PlaceholderText, QColor("#777777"));
			palette.setColor(group, QPalette::ToolTipBase, QColor("#ffffdc"));
			palette.setColor(group, QPalette::ToolTipText, QColor("#202020"));
			palette.setColor(group, QPalette::Link, QColor("#005a9e"));
			palette.setColor(group, QPalette::LinkVisited, QColor("#684694"));
		}
		for (const auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText, QPalette::Link, QPalette::LinkVisited}) {
			palette.setColor(QPalette::Disabled, role, QColor("#858585"));
		}
		palette.setColor(QPalette::Disabled, QPalette::Highlight, QColor("#d2d2d2"));
		palette.setColor(QPalette::Disabled, QPalette::HighlightedText, QColor("#858585"));
		return palette;
	}

	QString saveHomePicture(Profile *profile, int id, const QString &name, const QString &tags)
	{
		Image image(profile->getSites().value("danbooru.donmai.us"), {{"id", QString::number(id)}, {"name", name},
			{"file_url", "https://test.invalid/" + QString::number(id) + ".png"}, {"tags", tags}}, profile);
		const QStringList expectedTags = tags.split(' ', Qt::SkipEmptyParts);
		QList<Tag> imageTags;
		for (const auto &tag : expectedTags) { imageTags.append(Tag(tag)); }
		image.setTags(imageTags);
		QPixmap preview(320, 200);
		QPainter painter(&preview);
		painter.setRenderHint(QPainter::Antialiasing);
		painter.setPen(Qt::NoPen);
		const bool city = tags.contains("city") || name.contains("City");
		QLinearGradient sky(0, 0, 0, 200);
		sky.setColorAt(0, QColor(city ? "#262546" : "#91bbca"));
		sky.setColorAt(1, QColor(city ? "#a16d95" : "#ead4aa"));
		painter.fillRect(preview.rect(), sky);
		painter.setBrush(QColor(city ? "#eee2ce" : "#fff1c8"));
		painter.drawEllipse(QRectF(232 - id % 4 * 12, 24, 33, 33));
		if (city) {
			for (int building = 0; building < 9; ++building) {
				const int x = building * 39 - 10;
				const int top = 58 + (building * 19 + id * 7) % 64;
				painter.setBrush(QColor(building % 2 ? "#343449" : "#25283a"));
				painter.drawRect(x, top, 34, 142);
				painter.setBrush(QColor(building % 3 ? "#ecc98f" : "#ce9dd5"));
				for (int y = top + 9; y < 167; y += 16) {
					for (int window = 0; window < 3; ++window) {
						if ((window + y + id) % 3) { painter.drawRect(x + 5 + window * 9, y, 4, 7); }
					}
				}
			}
			painter.fillRect(0, 172, 320, 28, QColor("#202638"));
			painter.setPen(QPen(QColor("#d798ba"), 2));
			painter.drawLine(0, 178, 320, 178);
			painter.setPen(QPen(QColor("#edcf9c"), 2));
			for (int x = 15; x < 320; x += 48) { painter.drawLine(x, 191, x + 21, 191); }
		} else {
			painter.setBrush(QColor("#6d9588"));
			painter.drawPolygon(QPolygonF({{0, 127}, {64, 52}, {127, 133}, {188, 79}, {320, 137}, {320, 200}, {0, 200}}));
			painter.setBrush(QColor("#416b5d"));
			painter.drawPolygon(QPolygonF({{0, 136}, {71, 112}, {155, 154}, {252, 112}, {320, 141}, {320, 200}, {0, 200}}));
			QPainterPath river;
			river.moveTo(179, 121);
			river.cubicTo(128, 145, 267, 167, 211, 200);
			river.lineTo(127, 200);
			river.cubicTo(221, 172, 121, 148, 168, 121);
			river.closeSubpath();
			painter.fillPath(river, QColor("#c8e2dd"));
			for (int x : {24, 59, 279, 304}) {
				const int top = 91 + (x + id) % 37;
				painter.setBrush(QColor("#274c40"));
				painter.drawRect(x - 2, top + 31, 4, 47);
				painter.drawPolygon(QPolygonF({{qreal(x), qreal(top)}, {qreal(x - 20), qreal(top + 55)}, {qreal(x + 20), qreal(top + 55)}}));
			}
		}
		painter.end();
		image.setPreviewImage(preview);
		const QString key = profile->library()->saveImage(image);
		REQUIRE(!key.isEmpty());
		REQUIRE(profile->library()->entry(key).tags() == expectedTags);
		REQUIRE(profile->library()->entry(key).image.value("name").toString() == name);
		return key;
	}

	QListWidgetItem *homeItem(QListWidget *grid, const QString &key)
	{
		for (int index = 0; index < grid->count(); ++index) {
			if (grid->item(index)->data(Qt::UserRole).toString() == key) { return grid->item(index); }
		}
		return nullptr;
	}
}

TEST_CASE("Home keeps recommendation actions inside their explicit preference scope", "[library][home]")
{
	QTemporaryDir directory;
	REQUIRE(directory.isValid());
	const QScopedPointer<Profile> profile(makeLibraryProfile(directory.path()));
	const QString forest = saveHomePicture(profile.data(), 201, "Forest light", "forest green");
	const QString city = saveHomePicture(profile.data(), 202, "City lights", "city night");
	const QString forestMatch = saveHomePicture(profile.data(), 203, "Forest river", "forest river");
	const QString cityMatch = saveHomePicture(profile.data(), 204, "City neon", "city neon");
	const QString memberMatch = saveHomePicture(profile.data(), 205, "City palette", "city palette");
	REQUIRE(profile->library()->setFavorite(forest, true));
	const qint64 collection = profile->library()->createCollection("Night inspiration <private>");
	REQUIRE(collection > 0);
	REQUIRE(profile->library()->addToCollection(city, collection));
	REQUIRE(profile->library()->addToCollection(memberMatch, collection));
	REQUIRE(profile->library()->setFavorite(city, true, collection));
	const bool capture = !qEnvironmentVariable("GRABBER_HOME_SCREENSHOT").isEmpty();
	const QPalette initialPalette = qApp->palette();
	const QString initialStyleSheet = qApp->styleSheet();
	auto restoreAppearance = qScopeGuard([&]() {
		if (capture) { qApp->setStyleSheet(initialStyleSheet); qApp->setPalette(initialPalette); }
	});
	if (capture) {
		// Initialize before creating children: stylesheet widgets cache their inherited palette.
		QApplication::setStyle("Fusion");
		QApplication::setPalette(lightCapturePalette());
	}
	HomeTab home(profile.data(), nullptr);
	home.resize(1200, 880);
	home.show();
	QApplication::processEvents();
	auto *grid = home.findChild<QListWidget*>("homeGrid");
	auto *scope = home.findChild<QComboBox*>("homeScope");
	auto *mode = home.findChild<QComboBox*>("homeMode");
	REQUIRE(grid != nullptr);
	REQUIRE(scope != nullptr);
	REQUIRE(mode != nullptr);
	REQUIRE(grid->count() == 1);
	REQUIRE(homeItem(grid, forestMatch) != nullptr);
	REQUIRE(grid->selectedItems().isEmpty());
	REQUIRE_FALSE(home.findChild<QToolButton*>("libraryLike")->isVisible());
	REQUIRE_FALSE(home.findChild<QPushButton*>("homeViewPicture")->isVisible());
	REQUIRE(grid->item(0)->text().isEmpty());
	REQUIRE(grid->item(0)->data(Qt::AccessibleTextRole).toString() == "Forest river");
	scope->setCurrentIndex(scope->findData(collection));
	REQUIRE(QTest::qWaitFor([grid]() { return grid->count() == 2; }, 3000));
	REQUIRE(homeItem(grid, forestMatch) == nullptr);
	REQUIRE(homeItem(grid, cityMatch) != nullptr);
	grid->setCurrentItem(homeItem(grid, cityMatch));
	grid->currentItem()->setSelected(true);
	auto *like = home.findChild<QToolButton*>("libraryLike");
	auto *add = home.findChild<QPushButton*>("homeAddToCollection");
	auto *view = home.findChild<QPushButton*>("homeViewPicture");
	REQUIRE(like != nullptr);
	REQUIRE_FALSE(like->isEnabled());
	REQUIRE(add->isVisible());
	REQUIRE(view->text().contains("Library-wide"));
	REQUIRE(home.findChild<QLabel*>("homeHint")->textFormat() == Qt::PlainText);
	const QString collectionScreenshot = qEnvironmentVariable("GRABBER_HOME_SCREENSHOT");
	if (!collectionScreenshot.isEmpty()) {
		const QString stem = collectionScreenshot.endsWith(".png", Qt::CaseInsensitive) ? collectionScreenshot.left(collectionScreenshot.size() - 4) : collectionScreenshot;
		QApplication::processEvents();
		REQUIRE(home.grab().save(stem + "-collection.png"));
	}
	QSignalSpy picture(&home, &HomeTab::pictureRequested);
	QTest::mouseClick(view, Qt::LeftButton);
	REQUIRE(picture.size() == 1);
	REQUIRE(picture.at(0).at(0).toString() == cityMatch);
	REQUIRE(picture.at(0).at(2).toLongLong() == 0);
	QTest::mouseClick(like, Qt::LeftButton);
	REQUIRE_FALSE(profile->library()->entry(cityMatch).liked);
	REQUIRE_FALSE(profile->library()->contains(cityMatch, collection));
	QTest::mouseClick(add, Qt::LeftButton);
	REQUIRE(QTest::qWaitFor([like]() { return like->isEnabled(); }, 3000));
	REQUIRE(profile->library()->contains(cityMatch, collection));
	REQUIRE(grid->currentItem()->data(Qt::UserRole).toString() == cityMatch);
	REQUIRE_FALSE(add->isVisible());
	QTest::mouseClick(view, Qt::LeftButton);
	REQUIRE(picture.last().at(2).toLongLong() == collection);
	const QStringList viewerKeys = picture.last().at(1).toStringList();
	for (const auto &key : viewerKeys) { REQUIRE(profile->library()->contains(key, collection)); }
	QPointer<ImageLibraryActions> actions = home.findChild<ImageLibraryActions*>();
	QTest::mouseClick(like, Qt::LeftButton);
	REQUIRE(profile->library()->entry(cityMatch, collection).liked);
	REQUIRE_FALSE(profile->library()->entry(cityMatch).liked);
	REQUIRE(!actions.isNull()); // The queued refresh never deletes the action sender.
	REQUIRE(QTest::qWaitFor([grid]() { return grid->count() == 1; }, 3000));
	grid->setCurrentItem(homeItem(grid, memberMatch));
	grid->currentItem()->setSelected(true);
	REQUIRE(profile->library()->setNotes(memberMatch, "Keep the palette", collection));
	QApplication::processEvents();
	REQUIRE(grid->currentItem()->data(Qt::UserRole).toString() == memberMatch);
	QTest::mouseClick(home.findChild<QPushButton*>("homeHideSuggestion"), Qt::LeftButton);
	REQUIRE(QTest::qWaitFor([grid]() { return grid->count() == 0; }, 3000));
	QTest::mouseClick(home.findChild<QPushButton*>("homeRestoreHidden"), Qt::LeftButton);
	REQUIRE(QTest::qWaitFor([grid]() { return grid->count() == 1; }, 3000));
	mode->setCurrentIndex(1);
	REQUIRE(QTest::qWaitFor([grid]() { return grid->count() == 3; }, 3000));
	QSignalSpy browse(&home, &HomeTab::libraryRequested);
	QTest::mouseClick(home.findChild<QPushButton*>("homeOpenLibrary"), Qt::LeftButton);
	REQUIRE(browse.size() == 1);
	REQUIRE(browse.first().at(0).toLongLong() == collection);
	REQUIRE(profile->library()->removeCollection(collection));
	REQUIRE(QTest::qWaitFor([scope]() { return scope->currentData().toLongLong() == 0; }, 3000));
	REQUIRE(grid->count() == 5);
	REQUIRE(profile->library()->entry(forest).favorite);
	REQUIRE_FALSE(profile->library()->entry(cityMatch).liked);
	REQUIRE(home.findChild<QLabel*>("homeCoverage")->text().contains("0 pictures indexed"));
	REQUIRE_FALSE(home.findChild<QPushButton*>("homeUpdateIndex")->isEnabled());
	REQUIRE(home.findChild<QPushButton*>("homeSetupModel")->isEnabled());
	const QString screenshot = qEnvironmentVariable("GRABBER_HOME_SCREENSHOT");
	if (!screenshot.isEmpty()) {
		ThemeLoader theme(QFileInfo(QString::fromUtf8(__FILE__)).absolutePath() + "/../../dist/common/themes/", profile->getSettings());
		const QString oldStyleSheet = qApp->styleSheet();
		const QPalette oldPalette = qApp->palette();
		const QString stem = screenshot.endsWith(".png", Qt::CaseInsensitive) ? screenshot.left(screenshot.size() - 4) : screenshot;
		REQUIRE(theme.setTheme("Default"));
		home.resize(1200, 880);
		QApplication::processEvents();
		REQUIRE(home.size() == QSize(1200, 880));
		REQUIRE(home.grab().save(stem + "-light.png"));
		REQUIRE(theme.setTheme("Tokyo Night"));
		QApplication::processEvents();
		REQUIRE(home.grab().save(screenshot));
		home.resize(900, 700);
		QApplication::processEvents();
		INFO("Home compact=" << home.width() << "x" << home.height() << " minimum=" << home.minimumSizeHint().width()
			<< " viewport=" << grid->viewport()->width() << " horizontal=" << grid->horizontalScrollBar()->maximum());
		REQUIRE(home.grab().save(stem + "-compact.png"));
		REQUIRE(home.size() == QSize(900, 700));
		REQUIRE(home.minimumSizeHint().width() <= 900);
		REQUIRE(grid->viewport()->width() > grid->gridSize().width());
		REQUIRE(grid->horizontalScrollBar()->maximum() == 0);
		const QRect viewportRect(grid->viewport()->mapTo(&home, QPoint()), grid->viewport()->size());
		REQUIRE(home.rect().contains(viewportRect));
		for (auto *button : home.findChildren<QAbstractButton*>()) {
			if (!button->isVisible()) { continue; }
			INFO("Visible button " << button->objectName().toStdString());
			REQUIRE(home.rect().contains(QRect(button->mapTo(&home, QPoint()), button->size())));
		}
		qApp->setStyleSheet(oldStyleSheet);
		qApp->setPalette(oldPalette);
	}
}

TEST_CASE("Home explains tagless ratings and keeps recent cards bounded", "[library][home]")
{
	QTemporaryDir directory;
	REQUIRE(directory.isValid());
	const QScopedPointer<Profile> profile(makeLibraryProfile(directory.path()));
	const QString seed = saveHomePicture(profile.data(), 301, "Untitled favorite", {});
	REQUIRE(profile->library()->setFavorite(seed, true));
	for (int index = 0; index < 60; ++index) { REQUIRE(!saveHomePicture(profile.data(), 302 + index, "Saved picture", {}).isEmpty()); }
	HomeTab home(profile.data(), nullptr);
	auto *empty = home.findChild<QLabel*>("homeEmpty");
	REQUIRE(empty->text().contains("likes and favorites are saved"));
	REQUIRE(empty->text().contains("no tags or visual index"));
	auto *mode = home.findChild<QComboBox*>("homeMode");
	auto *grid = home.findChild<QListWidget*>("homeGrid");
	mode->setCurrentIndex(1);
	REQUIRE(QTest::qWaitFor([grid]() { return grid->count() == 48; }, 3000));
	REQUIRE(grid->item(0)->text().isEmpty());
	REQUIRE(grid->item(0)->toolTip().contains("Needs tags"));
	auto *count = home.findChild<QComboBox*>("homePictureCount");
	count->setCurrentIndex(count->findData(24));
	REQUIRE(QTest::qWaitFor([grid]() { return grid->count() == 24; }, 3000));
	count->setCurrentIndex(count->findData(96));
	REQUIRE(QTest::qWaitFor([grid]() { return grid->count() == 61; }, 3000));
	auto *density = home.findChild<QComboBox*>("homeDensity");
	density->setCurrentIndex(0);
	REQUIRE(grid->iconSize() == QSize(128, 128));
	REQUIRE(grid->gridSize() == QSize(140, 140));
	density->setCurrentIndex(2);
	REQUIRE(grid->iconSize() == QSize(256, 256));
	REQUIRE(profile->getSettings()->value("Gallery/density").toInt() == 2);
	REQUIRE(profile->getSettings()->value("Home/pictureCount").toInt() == 96);
	REQUIRE(profile->library()->entry(seed).favorite);
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

TEST_CASE("Home refresh rotates suitable pictures and reveals actions on keyboard selection", "[library][home]")
{
	QTemporaryDir directory;
	REQUIRE(directory.isValid());
	const QScopedPointer<Profile> profile(makeLibraryProfile(directory.path()));
	const QString seed = saveHomePicture(profile.data(), 501, "Forest favorite", "forest green");
	REQUIRE(profile->library()->setFavorite(seed, true));
	for (int index = 0; index < 60; ++index) {
		saveHomePicture(profile.data(), 502 + index, "Forest " + QString::number(index), "forest green");
	}
	HomeTab home(profile.data(), nullptr);
	home.resize(900, 700);
	home.show();
	QApplication::processEvents();
	auto *grid = home.findChild<QListWidget*>("homeGrid");
	auto *refresh = home.findChild<QPushButton*>("homeRefreshSuggestions");
	REQUIRE(grid->count() == 48);
	QStringList original;
	for (int i = 0; i < grid->count(); ++i) { original.append(grid->item(i)->data(Qt::UserRole).toString()); }
	REQUIRE_FALSE(home.findChild<QToolButton*>("libraryLike")->isVisible());
	QTest::mouseClick(refresh, Qt::LeftButton);
	REQUIRE(QTest::qWaitFor([grid, original]() {
		QStringList refreshed;
		for (int i = 0; i < grid->count(); ++i) { refreshed.append(grid->item(i)->data(Qt::UserRole).toString()); }
		return refreshed != original;
	}, 3000));
	REQUIRE(grid->selectedItems().isEmpty());
	grid->setFocus();
	QTest::keyClick(grid, Qt::Key_Down);
	REQUIRE(QTest::qWaitFor([grid]() { return !grid->selectedItems().isEmpty(); }, 3000));
	REQUIRE(home.findChild<QToolButton*>("libraryLike")->isVisible());
	REQUIRE(home.findChild<QToolButton*>("libraryFavorite")->isVisible());
	REQUIRE(home.findChild<QLabel*>("homeSelectionHint")->text().contains("Shared tags"));
	grid->clearSelection();
	REQUIRE_FALSE(home.findChild<QToolButton*>("libraryLike")->isVisible());
	REQUIRE_FALSE(home.findChild<QPushButton*>("homeHideSuggestion")->isVisible());
	REQUIRE_FALSE(profile->library()->entry(seed).liked);
	REQUIRE(profile->library()->entry(seed).favorite);
}

TEST_CASE("Home discovers real source candidates when every saved picture is rated", "[library][home][discovery]")
{
	auto clearReplies = qScopeGuard([]() { CustomNetworkAccessManager::NextFiles.clear(); });
	QTemporaryDir directory;
	REQUIRE(directory.isValid());
	const QScopedPointer<Profile> profile(makeLibraryProfile(directory.path()));
	auto *site = profile->getSites().value("danbooru.donmai.us");
	REQUIRE(site != nullptr);
	site->setSetting("sources/usedefault", false, true);
	site->setSetting("sources/source_1", "Json", "Xml");
	site->setSetting("sources/source_2", "", "Json");
	site->setSetting("sources/source_3", "", "Regex");
	site->setSetting("sources/source_4", "", "Rss");
	site->loadConfig();
	profile->getSettings()->setValue("sites", QStringList{site->url()});
	const QString globalSeed = saveHomePicture(profile.data(), 801, "Forest favorite", "forest");
	REQUIRE(profile->library()->setFavorite(globalSeed, true));
	qint64 collection = 0;
	QString queryTag = "forest";
	bool tagless = false;
	bool failFirst = false;
	bool cancelBeforeReply = false;
	SECTION("Failed source requests are visible and refresh can recover") { failFirst = true; }
	SECTION("Changing mode cancels pending discovery without stale results") { cancelBeforeReply = true; }
	SECTION("Library-wide discovery saves the actual source only when rated") {}
	SECTION("Collection discovery requires membership and never changes global ratings")
	{
		collection = profile->library()->createCollection("City only");
		const QString scopedSeed = saveHomePicture(profile.data(), 802, "City favorite", "city");
		REQUIRE(profile->library()->setLiked(scopedSeed, true));
		REQUIRE(profile->library()->addToCollection(scopedSeed, collection));
		REQUIRE(profile->library()->setFavorite(scopedSeed, true, collection));
		queryTag = "city";
		tagless = true;
	}
	HomeTab home(profile.data(), nullptr);
	home.resize(1100, 850);
	home.show();
	QApplication::processEvents();
	auto *grid = home.findChild<QListWidget*>("homeGrid");
	REQUIRE(grid != nullptr);
	REQUIRE(grid->count() == 0);
	if (collection > 0) {
		auto *scope = home.findChild<QComboBox*>("homeScope");
		scope->setCurrentIndex(scope->findData(collection));
		QApplication::processEvents();
	}
	const QString responsePath = directory.filePath("discovery.json");
	QFile response(responsePath);
	REQUIRE(response.open(QIODevice::WriteOnly));
	const QJsonArray payload {QJsonObject {
		{"id", 901}, {"md5", "1234567890abcdef1234567890abcdef"},
		{"file_url", "https://test.invalid/discovered.png"}, {"tag_string", tagless ? QString() : queryTag}
	}};
	REQUIRE(response.write(QJsonDocument(payload).toJson()) > 0);
	response.close();
	const QString previewPath = directory.filePath("discovery-preview.png");
	QImage preview(4, 4, QImage::Format_RGB32);
	preview.fill(Qt::blue);
	REQUIRE(preview.save(previewPath));
	auto queueResponse = [&]() {
		CustomNetworkAccessManager::NextFiles.enqueue(responsePath);
		CustomNetworkAccessManager::NextFiles.enqueue(previewPath);
	};
	if (failFirst) { CustomNetworkAccessManager::NextFiles.enqueue("500"); }
	else { queueResponse(); }
	auto *mode = home.findChild<QComboBox*>("homeMode");
	mode->setCurrentIndex(2);
	if (cancelBeforeReply) {
		home.reload();
		QPointer<Page> cancelled = home.findChild<Page*>();
		REQUIRE(!cancelled.isNull());
		mode->setCurrentIndex(1);
		REQUIRE(QTest::qWaitFor([&cancelled]() { return cancelled.isNull(); }, 3000));
		REQUIRE(grid->count() == 1);
		REQUIRE(grid->item(0)->data(Qt::UserRole).toString() == globalSeed);
		CustomNetworkAccessManager::NextFiles.clear();
		queueResponse();
		mode->setCurrentIndex(2);
	}
	if (failFirst) {
		auto *hint = home.findChild<QLabel*>("homeHint");
		REQUIRE(QTest::qWaitFor([hint]() { return hint->text().contains("could not load this topic"); }, 5000));
		REQUIRE(grid->count() == 0);
		REQUIRE(hint->text().contains(site->url()));
		queueResponse();
		QTest::mouseClick(home.findChild<QPushButton*>("homeRefreshSuggestions"), Qt::LeftButton);
	}
	REQUIRE(QTest::qWaitFor([grid, queryTag]() {
		return grid->count() == 1 && grid->item(0)->toolTip().contains("Searched " + queryTag)
			&& !grid->item(0)->toolTip().contains("Preview unavailable or still loading");
	}, 5000));
	const QString candidate = grid->item(0)->data(Qt::UserRole).toString();
	REQUIRE_FALSE(candidate.isEmpty());
	REQUIRE_FALSE(profile->library()->contains(candidate));
	REQUIRE(grid->item(0)->text().isEmpty());
	REQUIRE(grid->item(0)->toolTip().contains("Searched " + queryTag));
	REQUIRE(grid->item(0)->toolTip().contains("not been analyzed by local AI"));
	if (tagless) { REQUIRE(grid->item(0)->toolTip().contains("Tags unavailable")); }
	grid->setCurrentRow(0);
	grid->currentItem()->setSelected(true);
	auto *like = home.findChild<QToolButton*>("libraryLike");
	REQUIRE(like->isVisible());
	REQUIRE(like->isEnabled() == (collection == 0));
	if (collection > 0) {
		QTest::mouseClick(like, Qt::LeftButton);
		REQUIRE_FALSE(profile->library()->contains(candidate));
		QTest::mouseClick(home.findChild<QPushButton*>("homeAddToCollection"), Qt::LeftButton);
		REQUIRE(QTest::qWaitFor([like]() { return like->isEnabled(); }, 3000));
		REQUIRE(profile->library()->contains(candidate, collection));
	}
	QTest::mouseClick(like, Qt::LeftButton);
	const auto saved = profile->library()->entry(candidate, collection);
	REQUIRE(saved.liked);
	REQUIRE_FALSE(saved.favorite);
	REQUIRE(saved.image.value("website").toString() == site->url());
	REQUIRE(saved.image.value("id").toString() == "901");
	REQUIRE(saved.tags() == (tagless ? QStringList() : QStringList{queryTag}));
	if (collection > 0) { REQUIRE_FALSE(profile->library()->entry(candidate).liked); }
	REQUIRE(QTest::qWaitFor([grid]() { return grid->count() == 0; }, 3000));
	REQUIRE(profile->library()->entry(globalSeed).favorite);
	REQUIRE_FALSE(profile->library()->entry(globalSeed).liked);
	home.findChild<QComboBox*>("homeMode")->setCurrentIndex(1);
	REQUIRE(QTest::qWaitFor([grid]() { return grid->count() >= 2; }, 3000));
}
