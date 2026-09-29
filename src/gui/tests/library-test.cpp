#include <QApplication>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QScopedPointer>
#include <QTemporaryDir>
#include <QFileInfo>
#include <QTimer>
#include "theme-loader.h"
#include <QTest>
#include <QToolButton>
#include <QTreeWidget>
#include <QInputDialog>
#include "image-context-menu.h"
#include "image-library-actions.h"
#include "models/image.h"
#include "models/library-store.h"
#include "models/profile.h"
#include "tabs/library-tab.h"
#include "catch.h"
#include "source-helpers.h"


TEST_CASE("Library actions synchronize real widgets and scoped SQLite state", "[library]")
{
	QTemporaryDir directory;
	REQUIRE(directory.isValid());
	const QScopedPointer<Profile> profile(makeLibraryProfile(directory.path()));
	Site *site = profile->getSites().value("danbooru.donmai.us");
	REQUIRE(site != nullptr);
	auto image = QSharedPointer<Image>::create(site, QMap<QString, QString> {
		{ "id", "100" }, { "name", "Forest light" }, { "file_url", "https://test.invalid/forest.png" },
		{ "page_url", "https://danbooru.donmai.us/posts/100" }, { "tags", "forest green" }
	}, profile.data());
	QPixmap preview(320, 200);
	preview.fill(QColor("#406b59"));
	image->setPreviewImage(preview);
	ThemeLoader theme(QFileInfo(QString::fromUtf8(__FILE__)).absolutePath() + "/../../dist/common/themes/", profile->getSettings());
	if (!qEnvironmentVariableIsEmpty("GRABBER_LIBRARY_SCREENSHOT")) {
		QApplication::setStyle("Fusion");
		REQUIRE(theme.setTheme("Tokyo Night"));
	}
	ImageLibraryActions searchActions(profile.data(), image, nullptr, 0, true);
	ImageLibraryActions viewerActions(profile.data(), image);
	LibraryTab library(profile.data(), nullptr);
	library.resize(1180, 740);
	library.show();
	searchActions.show();
	viewerActions.show();
	QApplication::processEvents();
	auto *like = searchActions.findChild<QToolButton*>("libraryLike");
	auto *star = viewerActions.findChild<QToolButton*>("libraryFavorite");
	REQUIRE(like != nullptr);
	REQUIRE(star != nullptr);
	REQUIRE(like->text().startsWith(QChar(0x2661))); // U+2661, not a legacy-codepage garble.
	QTest::mouseClick(like, Qt::LeftButton);
	QApplication::processEvents();
	const QString key = LibraryStore::imageKey(*image);
	REQUIRE(profile->library()->entry(key).liked);
	REQUIRE(viewerActions.findChild<QToolButton*>("libraryLike")->isChecked());
	auto *grid = library.findChild<QListWidget*>("libraryGrid");
	REQUIRE(grid->count() == 1);
	REQUIRE(grid->item(0)->icon().pixmap(QSize(224, 160), QIcon::Selected).toImage().pixelColor(112, 70) == QColor("#406b59"));
	QTest::mouseClick(star, Qt::LeftButton);
	QApplication::processEvents();
	REQUIRE(searchActions.findChild<QToolButton*>("libraryFavorite")->isChecked());
	REQUIRE(profile->library()->entry(key).favorite);
	const qint64 collection = profile->library()->createCollection("Wallpapers");
	REQUIRE(collection > 0);
	REQUIRE(profile->library()->addToCollection(key, collection));
	QApplication::processEvents();
	auto *sidebar = library.findChild<QTreeWidget*>("librarySidebar");
	auto *collectionItem = sidebar->topLevelItem(5)->child(0);
	REQUIRE(collectionItem->data(0, Qt::UserRole).toLongLong() == collection);
	sidebar->setCurrentItem(collectionItem);
	QApplication::processEvents();
	REQUIRE(grid->count() == 1);
	grid->item(0)->setSelected(true);
	auto *libraryStar = library.findChild<QToolButton*>("libraryFavorite");
	REQUIRE(!libraryStar->isChecked()); // Global preference is not inherited by a collection.
	QTest::mouseClick(libraryStar, Qt::LeftButton);
	QApplication::processEvents();
	REQUIRE(profile->library()->entry(key, collection).favorite);
	REQUIRE(!profile->library()->entry(key, collection).liked);
	REQUIRE(profile->library()->entry(key).liked);
	ImageContextMenu menu(profile->getSettings(), image, nullptr);
	auto *menuLike = menu.findChild<QAction*>("libraryLikeAction");
	REQUIRE(menuLike != nullptr);
	REQUIRE(menuLike->isChecked());
	menuLike->trigger();
	QApplication::processEvents();
	REQUIRE(!like->isChecked());
	REQUIRE(profile->library()->entry(key, collection).favorite);
	library.findChild<QLineEdit*>("librarySearch")->setText("missing tag");
	QApplication::processEvents();
	REQUIRE(grid->count() == 0);
	library.findChild<QLineEdit*>("librarySearch")->clear();
	QApplication::processEvents();
	REQUIRE(grid->count() == 1);
	const QString screenshot = qEnvironmentVariable("GRABBER_LIBRARY_SCREENSHOT");
	if (!screenshot.isEmpty()) {
		grid->item(0)->setSelected(true);
		QApplication::processEvents();
		REQUIRE(library.grab().save(screenshot));
	}
	REQUIRE(profile->library()->removeCollection(collection));
	QApplication::processEvents();
	REQUIRE(grid->count() == 1);
	REQUIRE(profile->library()->contains(key));
	auto second = QSharedPointer<Image>::create(site, QMap<QString, QString> {
		{ "id", "101" }, { "name", "Moonlit hills" }, { "file_url", "https://test.invalid/hills.png" }
	}, profile.data());
	second->setPreviewImage(preview);
	const QString secondKey = profile->library()->saveImage(*second);
	REQUIRE(!secondKey.isEmpty());
	QApplication::processEvents();
	REQUIRE(grid->count() == 2);
	grid->selectAll();
	QTest::mouseClick(libraryStar, Qt::LeftButton);
	QApplication::processEvents();
	REQUIRE(profile->library()->entry(key).favorite);
	REQUIRE(profile->library()->entry(secondKey).favorite);
	QTest::mouseClick(libraryStar, Qt::LeftButton);
	QApplication::processEvents();
	REQUIRE(!profile->library()->entry(key).favorite);
	REQUIRE(!profile->library()->entry(secondKey).favorite);
	bool foundDialog = false;
	QTimer::singleShot(0, [&foundDialog]() {
		auto *dialog = qobject_cast<QInputDialog*>(QApplication::activeModalWidget());
		if (dialog != nullptr) {
			foundDialog = true;
			dialog->setTextValue("Sketch references");
			dialog->accept();
		}
	});
	QTest::mouseClick(library.findChild<QPushButton*>("libraryNewCollection"), Qt::LeftButton);
	QApplication::processEvents();
	REQUIRE(foundDialog);
	REQUIRE(profile->library()->collections().size() == 1);
	REQUIRE(profile->library()->collections().first().name == "Sketch references");
	REQUIRE(grid->count() == 0);
	REQUIRE(library.findChild<QPushButton*>("libraryManageCollection")->isEnabled());
}
