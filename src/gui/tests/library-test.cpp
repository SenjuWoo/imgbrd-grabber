#include <QApplication>
#include <QComboBox>
#include <QFile>
#include <QScrollBar>
#include <QWheelEvent>
#include <QSet>
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
	REQUIRE(grid->item(0)->text().isEmpty());
	REQUIRE_FALSE(library.findChild<QToolButton*>("libraryLike")->isVisible());
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
	auto *collectionItem = sidebar->topLevelItem(8)->child(0);
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
	REQUIRE_FALSE(profile->library()->entry(key).liked);
	ImageContextMenu menu(profile->getSettings(), image, nullptr);
	auto *menuLike = menu.findChild<QAction*>("libraryLikeAction");
	REQUIRE(menuLike != nullptr);
	REQUIRE_FALSE(menuLike->isChecked());
	menuLike->trigger();
	QApplication::processEvents();
	REQUIRE(like->isChecked());
	REQUIRE_FALSE(star->isChecked());
	REQUIRE(profile->library()->entry(key, collection).favorite);
	library.findChild<QLineEdit*>("librarySearch")->setText("missing tag");
	REQUIRE(QTest::qWaitFor([grid]() { return grid->count() == 0; }, 3000));
	library.findChild<QLineEdit*>("librarySearch")->clear();
	REQUIRE(QTest::qWaitFor([grid]() { return grid->count() == 1; }, 3000));
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

TEST_CASE("Large Library galleries keep the final picture reachable", "[library][gallery]")
{
	QTemporaryDir directory;
	REQUIRE(directory.isValid());
	const QString catalog = qEnvironmentVariable("GRABBER_TEST_CATALOG");
	if (!catalog.isEmpty()) {
		REQUIRE(QFile::copy(catalog, directory.filePath("library.sqlite")));
	}
	const QScopedPointer<Profile> profile(makeLibraryProfile(directory.path()));
	if (catalog.isEmpty()) {
		Site *site = profile->getSites().value("danbooru.donmai.us");
		for (int index = 0; index < 650; ++index) {
			Image image(site, {{"id", QString::number(index + 1)}, {"name", QString(120, 'a')}, {"file_url", "https://test.invalid/" + QString::number(index) + ".png"}}, profile.data());
			QPixmap preview(index % 2 ? QSize(100, 160) : QSize(160, 100));
			preview.fill(Qt::blue);
			image.setPreviewImage(preview);
			REQUIRE(!profile->library()->saveImage(image).isEmpty());
		}
	}
	const int expected = profile->library()->entries().size();
	REQUIRE(expected >= 478);
	ThemeLoader theme(QFileInfo(QString::fromUtf8(__FILE__)).absolutePath() + "/../../dist/common/themes/", profile->getSettings());
	REQUIRE(theme.setTheme("Tokyo Night"));
	LibraryTab library(profile.data(), nullptr);
	for (int pageSize : {50, 100, 200}) {
	for (const QSize &size : {QSize(900, 660), QSize(1500, 820)}) {
		library.showView();
		auto *pageControl = library.findChild<QComboBox*>("libraryPageSize");
		REQUIRE(pageControl != nullptr);
		pageControl->setCurrentIndex(pageControl->findData(pageSize));
		library.resize(size);
		library.show();
		QApplication::processEvents();
		auto *grid = library.findChild<QListWidget*>("libraryGrid");
		QSet<QString> seen;
		auto *next = library.findChild<QPushButton*>("libraryNextPage");
		auto *previous = library.findChild<QPushButton*>("libraryPreviousPage");
		REQUIRE(next != nullptr);
		REQUIRE(previous != nullptr);
		do {
			REQUIRE(grid->count() <= pageSize);
			for (int index = 0; index < grid->count(); ++index) {
				const auto key = grid->item(index)->data(Qt::UserRole).toString();
				REQUIRE_FALSE(seen.contains(key));
				seen.insert(key);
			}
			if (!next->isEnabled()) { break; }
			QTest::mouseClick(next, Qt::LeftButton);
			QApplication::processEvents();
		} while (true);
		REQUIRE(seen.size() == expected);
		REQUIRE(previous->isEnabled());
		grid->scrollToBottom();
		QApplication::processEvents();
		auto *last = grid->item(grid->count() - 1);
		const QRect visible = grid->visualItemRect(last);
		INFO("count=" << grid->count() << " viewport=" << grid->viewport()->width() << "x" << grid->viewport()->height() << " scroll=" << grid->verticalScrollBar()->value() << "/" << grid->verticalScrollBar()->maximum() << " last=" << visible.x() << "," << visible.y() << "," << visible.width() << "," << visible.height());
		REQUIRE(grid->viewport()->rect().intersects(visible));
		REQUIRE(grid->itemAt(visible.center()) == last);
		grid->scrollToTop();
		for (int step = 0; step < 500 && grid->verticalScrollBar()->value() < grid->verticalScrollBar()->maximum(); ++step) {
			const QPointF pos = grid->viewport()->rect().center();
			QWheelEvent wheel(pos, grid->viewport()->mapToGlobal(pos.toPoint()), QPoint(), QPoint(0, -120), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
			QApplication::sendEvent(grid->viewport(), &wheel);
		}
		REQUIRE(grid->verticalScrollBar()->value() == grid->verticalScrollBar()->maximum());
		REQUIRE(grid->viewport()->rect().intersects(grid->visualItemRect(last)));
		library.findChild<QLineEdit*>("librarySearch")->setText("no_such_tag");
		REQUIRE(QTest::qWaitFor([grid]() { return grid->count() == 0; }, 3000));
		library.findChild<QLineEdit*>("librarySearch")->clear();
		REQUIRE(QTest::qWaitFor([grid, pageSize]() { return grid->count() == pageSize; }, 3000));
		REQUIRE_FALSE(previous->isEnabled());
	}
	}
}
