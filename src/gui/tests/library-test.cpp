#include <QApplication>
#include <QComboBox>
#include <QFile>
#include <QItemSelectionModel>
#include <QPainter>
#include <QRandomGenerator>
#include <QScrollBar>
#include <QWheelEvent>
#include <QSet>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
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
#include "ui/image-grid.h"
#include "catch.h"
#include "source-helpers.h"


namespace
{
	void selectRow(ImageGridView *grid, int row, bool clear = true)
	{
		grid->selectionModel()->select(grid->gridModel()->index(row), clear ? QItemSelectionModel::ClearAndSelect : QItemSelectionModel::Select);
	}
}

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
	auto *grid = library.findChild<ImageGridView*>("libraryGrid");
	auto *model = grid->gridModel();
	REQUIRE(QTest::qWaitFor([model]() { return model->rowCount() == 1; }, 3000));
	REQUIRE(model->item(0).liked);
	REQUIRE_FALSE(library.findChild<QToolButton*>("libraryLike")->isVisible());
	REQUIRE(model->data(model->index(0), ImageGridModel::PixmapRole).value<QPixmap>().toImage().pixelColor(100, 60) == QColor("#406b59"));
	QTest::mouseClick(star, Qt::LeftButton);
	QApplication::processEvents();
	REQUIRE(searchActions.findChild<QToolButton*>("libraryFavorite")->isChecked());
	REQUIRE(profile->library()->entry(key).favorite);
	const qint64 collection = profile->library()->createCollection("Wallpapers");
	REQUIRE(collection > 0);
	REQUIRE(profile->library()->addToCollection(key, collection));
	QApplication::processEvents();
	auto *sidebar = library.findChild<QTreeWidget*>("librarySidebar");
	auto *collectionItem = sidebar->topLevelItem(9)->child(0);
	REQUIRE(collectionItem->data(0, Qt::UserRole).toLongLong() == collection);
	sidebar->setCurrentItem(collectionItem);
	QApplication::processEvents();
	REQUIRE(model->rowCount() == 1);
	selectRow(grid, 0);
	QApplication::processEvents();
	auto *libraryStar = library.findChild<QToolButton*>("libraryFavorite");
	REQUIRE(libraryStar->isVisible());
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
	REQUIRE(QTest::qWaitFor([model]() { return model->rowCount() == 0; }, 3000));
	library.findChild<QLineEdit*>("librarySearch")->clear();
	REQUIRE(QTest::qWaitFor([model]() { return model->rowCount() == 1; }, 3000));
	const QString screenshot = qEnvironmentVariable("GRABBER_LIBRARY_SCREENSHOT");
	if (!screenshot.isEmpty()) {
		selectRow(grid, 0);
		QApplication::processEvents();
		REQUIRE(library.grab().save(screenshot));
	}
	REQUIRE(profile->library()->removeCollection(collection));
	QApplication::processEvents();
	REQUIRE(QTest::qWaitFor([model]() { return model->rowCount() == 1; }, 3000));
	REQUIRE(profile->library()->contains(key));
	auto second = QSharedPointer<Image>::create(site, QMap<QString, QString> {
		{ "id", "101" }, { "name", "Moonlit hills" }, { "file_url", "https://test.invalid/hills.png" }
	}, profile.data());
	second->setPreviewImage(preview);
	const QString secondKey = profile->library()->saveImage(*second);
	REQUIRE(!secondKey.isEmpty());
	REQUIRE(QTest::qWaitFor([model]() { return model->rowCount() == 2; }, 3000));
	QTest::mouseClick(library.findChild<QPushButton*>("librarySelectAll"), Qt::LeftButton);
	QApplication::processEvents();
	REQUIRE(grid->selectedKeys().size() == 2);
	REQUIRE(library.findChild<QLabel*>("librarySelectionCount")->text().contains("2"));
	QTest::mouseClick(libraryStar, Qt::LeftButton);
	QApplication::processEvents();
	REQUIRE(profile->library()->entry(key).favorite);
	REQUIRE(profile->library()->entry(secondKey).favorite);
	QTest::mouseClick(libraryStar, Qt::LeftButton);
	QApplication::processEvents();
	REQUIRE(!profile->library()->entry(key).favorite);
	REQUIRE(!profile->library()->entry(secondKey).favorite);
	// Rating changes update tiles in place: the selection survives.
	REQUIRE(grid->selectedKeys().size() == 2);
	QTest::mouseClick(library.findChild<QPushButton*>("libraryClearSelection"), Qt::LeftButton);
	REQUIRE(grid->selectedKeys().isEmpty());
	REQUIRE_FALSE(library.findChild<QToolButton*>("libraryLike")->isVisible());
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
	REQUIRE(QTest::qWaitFor([model]() { return model->rowCount() == 0; }, 3000));
	REQUIRE(library.findChild<QPushButton*>("libraryManageCollection")->isEnabled());
}

TEST_CASE("Library grid reaches every picture without pages and finds duplicates", "[library][gallery]")
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
			// Random blocks per picture so only the planted pair below looks alike.
			QImage preview(index % 2 ? QSize(100, 160) : QSize(160, 100), QImage::Format_RGB32);
			preview.fill(Qt::black);
			QRandomGenerator rng(quint32(index) + 1);
			QPainter painter(&preview);
			for (int block = 0; block < 14; ++block) {
				painter.fillRect(QRect(int(rng.bounded(preview.width())), int(rng.bounded(preview.height())), 10 + int(rng.bounded(60)), 10 + int(rng.bounded(60))), QColor::fromRgb(rng.generate()));
			}
			painter.end();
			image.setPreviewImage(QPixmap::fromImage(preview));
			REQUIRE(!profile->library()->saveImage(image).isEmpty());
		}
		Site *other = profile->getSites().value("danbooru.donmai.us");
		Image copy(other, {{"id", "9001"}, {"file_url", "https://mirror.invalid/copy.png"}}, profile.data());
		Image original(other, {{"id", "9000"}, {"file_url", "https://test.invalid/original.png"}}, profile.data());
		QImage art(300, 420, QImage::Format_RGB32);
		art.fill(QColor(30, 40, 90));
		for (int i = 0; i < 12; ++i) {
			QPainter painter(&art);
			painter.fillRect(QRect(i * 23, i * 31, 60, 50), QColor::fromHsv(i * 29, 220, 230));
		}
		original.setPreviewImage(QPixmap::fromImage(art));
		copy.setPreviewImage(QPixmap::fromImage(art.scaled(150, 210, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)));
		REQUIRE(!profile->library()->saveImage(original).isEmpty());
		REQUIRE(!profile->library()->saveImage(copy).isEmpty());
	}
	const int expected = profile->library()->entries().size();
	REQUIRE(expected >= 478);
	ThemeLoader theme(QFileInfo(QString::fromUtf8(__FILE__)).absolutePath() + "/../../dist/common/themes/", profile->getSettings());
	REQUIRE(theme.setTheme("Tokyo Night"));
	LibraryTab library(profile.data(), nullptr);
	for (const QSize &size : {QSize(900, 660), QSize(1500, 820)}) {
		library.showView();
		library.resize(size);
		library.show();
		QApplication::processEvents();
		auto *grid = library.findChild<ImageGridView*>("libraryGrid");
		REQUIRE(grid->gridModel()->rowCount() == expected);
		const QStringList keys = grid->gridModel()->keys();
		REQUIRE(QSet<QString>(keys.begin(), keys.end()).size() == expected);
		// Tiles fill the row: the remaining width is less than one more column.
		const int columns = grid->viewport()->width() / grid->gridSize().width();
		REQUIRE(columns >= 2);
		REQUIRE(grid->viewport()->width() - columns * grid->gridSize().width() < grid->gridSize().width());
		const QModelIndex last = grid->gridModel()->index(expected - 1);
		for (int step = 0; step < 2000 && grid->verticalScrollBar()->value() < grid->verticalScrollBar()->maximum(); ++step) {
			const QPointF pos = grid->viewport()->rect().center();
			QWheelEvent wheel(pos, grid->viewport()->mapToGlobal(pos.toPoint()), QPoint(), QPoint(0, -120), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
			QApplication::sendEvent(grid->viewport(), &wheel);
		}
		REQUIRE(grid->verticalScrollBar()->value() == grid->verticalScrollBar()->maximum());
		REQUIRE(grid->viewport()->rect().intersects(grid->visualRect(last)));
		REQUIRE(grid->indexAt(grid->visualRect(last).center()) == last);
		library.findChild<QLineEdit*>("librarySearch")->setText("no_such_tag");
		REQUIRE(QTest::qWaitFor([grid]() { return grid->gridModel()->rowCount() == 0; }, 3000));
		library.findChild<QLineEdit*>("librarySearch")->clear();
		REQUIRE(QTest::qWaitFor([grid, expected]() { return grid->gridModel()->rowCount() == expected; }, 3000));
	}
	if (catalog.isEmpty()) {
		library.showView(0, LibraryTab::Duplicates);
		QApplication::processEvents();
		auto *grid = library.findChild<ImageGridView*>("libraryGrid");
		REQUIRE(grid->gridModel()->rowCount() == 2);
	}
}
