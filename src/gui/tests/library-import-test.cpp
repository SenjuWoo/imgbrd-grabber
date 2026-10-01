#include <QApplication>
#include <QFile>
#include <QImage>
#include <QListWidget>
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
	sidebar->setCurrentItem(sidebar->topLevelItem(5)->child(0));
	QApplication::processEvents();
	int added = -1, duplicates = -1, failed = -1;
	QObject::connect(&library, &LibraryTab::importFinished, [&](int a, int d, int f) { added = a; duplicates = d; failed = f; });
	library.importPaths({files.path()});
	REQUIRE(QTest::qWaitFor([&]() { return !library.importing(); }, 15000));
	REQUIRE(added == 2);
	REQUIRE(duplicates == 1);
	REQUIRE(failed == 0);
	auto *grid = library.findChild<QListWidget*>("libraryGrid");
	REQUIRE(grid->count() == 2);
	REQUIRE(profile->library()->entries(collection).size() == 2);
	grid->item(0)->setSelected(true);
	const QString key = grid->item(0)->data(Qt::UserRole).toString();
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
