#include <QApplication>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include "models/library-importer.h"
#include "models/image.h"
#include "viewer/library-source-dialog.h"
#include <QItemSelectionModel>
#include "ui/image-grid.h"
#include <QProgressDialog>
#include <QPushButton>
#include <QLabel>
#include <QScopedPointer>
#include <QTemporaryDir>
#include <QFileInfo>
#include "theme-loader.h"
#include <QTest>
#include <QToolButton>
#include <QTreeWidget>
#include "models/library-store.h"
#include "models/profile.h"
#include "tabs/library-tab.h"
#include "catch.h"
#include "source-helpers.h"

TEST_CASE("Asynchronous folder import keeps the UI and collection preferences usable", "[library][import]")
{
	QTemporaryDir directory;
	QTemporaryDir files;
	const QScopedPointer<Profile> profile(makeLibraryProfile(directory.path()));
	const qint64 collection = profile->library()->createCollection("Local references");
	QImage image(120, 80, QImage::Format_RGB32);
	image.fill(Qt::red);
	REQUIRE(image.save(files.filePath("a.png")));
	REQUIRE(QFile::copy(files.filePath("a.png"), files.filePath("same.png")));
	image.fill(Qt::blue);
	REQUIRE(image.save(files.filePath("b.png")));
	ThemeLoader theme(QFileInfo(QString::fromUtf8(__FILE__)).absolutePath() + "/../../dist/common/themes/", profile->getSettings());
	if (!qEnvironmentVariableIsEmpty("GRABBER_IMPORT_SCREENSHOT")) {
		QApplication::setStyle("Fusion");
		REQUIRE(theme.setTheme("Tokyo Night"));
	}
	LibraryTab library(profile.data(), nullptr);
	library.resize(1180, 740);
	library.show();
	auto *sidebar = library.findChild<QTreeWidget*>("librarySidebar");
	sidebar->setCurrentItem(sidebar->topLevelItem(9)->child(0));
	QApplication::processEvents();
	int added = -1, duplicates = -1, failed = -1;
	QObject::connect(&library, &LibraryTab::importFinished, [&](int a, int d, int f) { added = a; duplicates = d; failed = f; });
	library.importPaths({files.path()});
	REQUIRE(QTest::qWaitFor([&]() { return !library.importing(); }, 15000));
	REQUIRE(added == 2);
	REQUIRE(duplicates == 1);
	REQUIRE(failed == 0);
	auto *grid = library.findChild<ImageGridView*>("libraryGrid");
	REQUIRE(grid->gridModel()->rowCount() == 2);
	REQUIRE(profile->library()->entries(collection).size() == 2);
	grid->selectionModel()->select(grid->gridModel()->index(0), QItemSelectionModel::ClearAndSelect);
	const QString key = grid->gridModel()->item(0).key;
	QTest::mouseClick(library.findChild<QToolButton*>("libraryFavorite"), Qt::LeftButton);
	QApplication::processEvents();
	REQUIRE(profile->library()->entry(key, collection).favorite);
	REQUIRE_FALSE(profile->library()->entry(key).favorite);
	library.importPaths({files.path()}, true);
	REQUIRE(QTest::qWaitFor([&]() { return !library.importing(); }, 15000));
	REQUIRE(added == 0);
	REQUIRE(duplicates == 3);
	REQUIRE(QFile::exists(files.filePath("a.png")));
	REQUIRE(profile->library()->entry(key, collection).favorite);
	REQUIRE(profile->library()->entry(key).localPaths.size() >= 2);
	const QString screenshot = qEnvironmentVariable("GRABBER_IMPORT_SCREENSHOT");
	if (!screenshot.isEmpty()) {
		REQUIRE(library.grab().save(screenshot));
	}
	library.importPaths({files.path()});
	auto *progress = library.findChild<QProgressDialog*>();
	REQUIRE(progress != nullptr);
	auto *cancel = progress->findChild<QPushButton*>();
	REQUIRE(cancel != nullptr);
	QTest::mouseClick(cancel, Qt::LeftButton);
	REQUIRE(QTest::qWaitFor([&]() { return !library.importing(); }, 15000));
	REQUIRE(profile->library()->entries().size() == 2);
	bool cancelled = false;
	for (const auto *label : library.findChildren<QLabel*>()) {
		cancelled = cancelled || label->text().contains("Cancelled; completed pictures were kept.");
	}
	REQUIRE(cancelled);
}

TEST_CASE("Metadata review views keep untagged ratings and source identification separate", "[library][import][metadata-review]")
{
	QTemporaryDir directory;
	const QScopedPointer<Profile> profile(makeLibraryProfile(directory.path()));
	QImage pixels(80, 60, QImage::Format_RGB32);
	pixels.fill(Qt::green);
	const QString path = directory.filePath("plain.png");
	REQUIRE(pixels.save(path));
	QFile sidecar(path + ".json");
	REQUIRE(sidecar.open(QIODevice::WriteOnly));
	sidecar.write("{bad json");
	sidecar.close();
	const QString key = profile->library()->saveLocalImage(LibraryImporter::inspect(path));
	REQUIRE(!key.isEmpty());
	REQUIRE(profile->library()->entry(key).tags().isEmpty());
	REQUIRE_FALSE(profile->library()->entry(key).metadataErrors().isEmpty());
	LibraryTab library(profile.data(), nullptr);
	library.resize(1180, 740);
	library.show();
	QApplication::processEvents();
	auto *grid = library.findChild<ImageGridView*>("libraryGrid");
	auto *sidebar = library.findChild<QTreeWidget*>("librarySidebar");
	auto count = [grid]() { return grid->gridModel()->rowCount(); };
	auto selectFirst = [grid]() { grid->selectionModel()->select(grid->gridModel()->index(0), QItemSelectionModel::ClearAndSelect); };
	sidebar->setCurrentItem(sidebar->topLevelItem(5)); // Needs tags.
	QApplication::processEvents();
	REQUIRE(count() == 1);
	selectFirst();
	QTest::mouseClick(library.findChild<QToolButton*>("libraryLike"), Qt::LeftButton);
	QApplication::processEvents();
	REQUIRE(profile->library()->entry(key).liked);
	selectFirst();
	QTest::mouseClick(library.findChild<QToolButton*>("libraryFavorite"), Qt::LeftButton);
	QApplication::processEvents();
	REQUIRE(profile->library()->entry(key).favorite);
	selectFirst();
	auto *find = library.findChild<QPushButton*>("libraryFindSource");
	REQUIRE(find->isEnabled());
	QTest::mouseClick(find, Qt::LeftButton);
	auto *dialog = library.findChild<LibrarySourceDialog*>("librarySourceDialog");
	REQUIRE(dialog != nullptr);
	REQUIRE(dialog->findChild<QPushButton*>("librarySourceSimilar") != nullptr);
	dialog->close();
	REQUIRE(sidecar.open(QIODevice::WriteOnly | QIODevice::Truncate));
	sidecar.write(R"({"tag_string_general":"recovered forest"})");
	sidecar.close();
	REQUIRE(profile->library()->saveLocalImage(LibraryImporter::inspect(path)) == key);
	QApplication::processEvents();
	REQUIRE(count() == 0); // Tags recovered, so this view no longer contains it.
	REQUIRE(profile->library()->entry(key).metadataErrors().isEmpty());
	REQUIRE(profile->library()->entry(key).tags().contains("recovered"));
	sidebar->setCurrentItem(sidebar->topLevelItem(6)); // Still needs source despite its tags.
	QApplication::processEvents();
	REQUIRE(count() == 1);
	Site *site = profile->getSites().value("danbooru.donmai.us");
	Image remote(site, {{"id", "912"}, {"tags", "forest"}, {"file_url", "https://test.invalid/source.png"}}, profile.data());
	REQUIRE(profile->library()->linkSource(key, remote, "User confirmed test candidate"));
	QApplication::processEvents();
	REQUIRE(count() == 0);
	REQUIRE_FALSE(profile->library()->entry(key).liked);
	REQUIRE(profile->library()->entry(key).favorite);
	REQUIRE(profile->library()->entry(key).tags().contains("recovered"));
	sidebar->setCurrentItem(sidebar->topLevelItem(7));
	QApplication::processEvents();
	REQUIRE(count() == 0);
}
