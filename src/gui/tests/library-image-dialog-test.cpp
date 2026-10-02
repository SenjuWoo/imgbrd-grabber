#include <QApplication>
#include <QColor>
#include <QFile>
#include <QGraphicsPixmapItem>
#include <QGraphicsScene>
#include <QGraphicsView>
#include <QImage>
#include <QFileInfo>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QScopedPointer>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTextDocument>
#include <QTest>
#include <QTimer>
#include <QToolButton>
#include "models/library-importer.h"
#include "models/library-store.h"
#include "models/profile.h"
#include "theme-loader.h"
#include "viewer/library-image-dialog.h"
#include "catch.h"
#include "source-helpers.h"


TEST_CASE("Offline Library viewer retains scoped actions and notes through navigation", "[library][import][offline-view]")
{
	QTemporaryDir directory;
	REQUIRE(directory.isValid());
	const QScopedPointer<Profile> profile(makeLibraryProfile(directory.path()));
	ThemeLoader theme(QFileInfo(QString::fromUtf8(__FILE__)).absolutePath() + "/../../dist/common/themes/", profile->getSettings());
	const QString screenshot = qEnvironmentVariable("GRABBER_OFFLINE_SCREENSHOT");
	if (!screenshot.isEmpty()) {
		QApplication::setStyle("Fusion");
		REQUIRE(theme.setTheme("Tokyo Night"));
	}
	QImage image(320, 200, QImage::Format_RGB32);
	image.fill(QColor("#49765b"));
	const QString firstPath = directory.filePath("forest.png");
	REQUIRE(image.save(firstPath));
	const auto firstImport = LibraryImporter::inspect(firstPath);
	REQUIRE(firstImport.error.isEmpty());
	const QString first = profile->library()->saveLocalImage(firstImport);
	REQUIRE(!first.isEmpty());
	image.fill(QColor("#655990"));
	const QString secondPath = directory.filePath("night.png");
	REQUIRE(image.save(secondPath));
	const auto secondImport = LibraryImporter::inspect(secondPath);
	REQUIRE(secondImport.error.isEmpty());
	const QString second = profile->library()->saveLocalImage(secondImport);
	REQUIRE(!second.isEmpty());
	const qint64 collection = profile->library()->createCollection("Offline favorites");
	REQUIRE(profile->library()->addToCollection(first, collection));
	REQUIRE(profile->library()->addToCollection(second, collection));
	QPointer<LibraryImageDialog> viewer = new LibraryImageDialog(profile.data(), {first, second}, first, collection);
	viewer->show();
	QApplication::processEvents();
	auto *view = viewer->findChild<QGraphicsView*>("libraryImageView");
	REQUIRE(view != nullptr);
	REQUIRE(view->scene()->items().size() == 1);
	REQUIRE(!viewer->findChild<QLabel*>("libraryImageStatus")->isVisible());
	auto *like = viewer->findChild<QToolButton*>("libraryLike");
	REQUIRE(like != nullptr);
	QTest::mouseClick(like, Qt::LeftButton);
	REQUIRE(profile->library()->entry(first, collection).liked);
	REQUIRE(!profile->library()->entry(first).liked);
	if (!screenshot.isEmpty()) {
		QApplication::processEvents();
		REQUIRE(viewer->grab().save(screenshot));
	}
	auto *notes = viewer->findChild<QPlainTextEdit*>("libraryImageNotes");
	notes->setFocus();
	QTest::keyClicks(notes, "Keep this color palette");
	QTest::mouseClick(viewer->findChild<QPushButton*>("libraryImageNext"), Qt::LeftButton);
	REQUIRE(viewer->property("libraryImageKey").toString() == second);
	REQUIRE(profile->library()->entry(first, collection).notes == "Keep this color palette");
	REQUIRE(notes->toPlainText().isEmpty());
	QSignalSpy locate(viewer, &LibraryImageDialog::locateRequested);
	QSignalSpy source(viewer, &LibraryImageDialog::sourceRequested);
	QTest::mouseClick(viewer->findChild<QPushButton*>("libraryImageLocate"), Qt::LeftButton);
	QTest::mouseClick(viewer->findChild<QPushButton*>("libraryImageFindSource"), Qt::LeftButton);
	REQUIRE(locate.size() == 1);
	REQUIRE(locate.at(0).at(0).toString() == second);
	REQUIRE(source.size() == 1);
	REQUIRE(source.at(0).at(0).toString() == second);
	REQUIRE(!viewer->findChild<QPushButton*>("libraryImageOpenSource")->isEnabled());
	for (int index = 0; index < 4; ++index) {
		QTest::mouseClick(viewer->findChild<QPushButton*>("libraryImagePrevious"), Qt::LeftButton);
		REQUIRE(viewer->property("libraryImageKey").toString() == first);
		QTest::mouseClick(viewer->findChild<QPushButton*>("libraryImageNext"), Qt::LeftButton);
		REQUIRE(viewer->property("libraryImageKey").toString() == second);
		REQUIRE(view->scene()->items().size() == 1);
	}
	// Shortcuts need an active window even when the native test runner starts hidden.
	QApplication::setActiveWindow(viewer);
	view->setFocus();
	QTest::keyClick(view, Qt::Key_Left);
	REQUIRE(viewer->property("libraryImageKey").toString() == first);
	notes->setFocus();
	QTest::keyClicks(notes, " with unsaved details");
	const QString draft = notes->toPlainText();
	REQUIRE(profile->library()->removeCollection(collection));
	REQUIRE(!like->isEnabled());
	REQUIRE(viewer->findChild<QLabel*>("libraryImageStatus")->text().contains("removed"));
	bool cancelledClose = false;
	QTimer::singleShot(0, [&cancelledClose]() {
		auto *message = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
		if (message) {
			message->button(QMessageBox::Cancel)->click();
			cancelledClose = true;
		}
	});
	viewer->reject();
	REQUIRE(cancelledClose);
	REQUIRE(!viewer.isNull());
	REQUIRE(viewer->isVisible());
	REQUIRE(notes->toPlainText() == draft);
	// Explicitly discard this temporary test draft to close without touching the user's clipboard.
	notes->document()->setModified(false);
	viewer->reject();
	QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
	REQUIRE(viewer.isNull());
}


TEST_CASE("Offline Library viewer falls back to its saved thumbnail when original disappears", "[library][import][offline-view]")
{
	QTemporaryDir directory;
	REQUIRE(directory.isValid());
	const QScopedPointer<Profile> profile(makeLibraryProfile(directory.path()));
	QImage image(160, 90, QImage::Format_RGB32);
	image.fill(QColor("#a46759"));
	const QString path = directory.filePath("missing-later.png");
	REQUIRE(image.save(path));
	const auto imported = LibraryImporter::inspect(path);
	REQUIRE(imported.error.isEmpty());
	const QString key = profile->library()->saveLocalImage(imported);
	REQUIRE(!key.isEmpty());
	REQUIRE(QFile::remove(path));
	QPointer<LibraryImageDialog> viewer = new LibraryImageDialog(profile.data(), {key}, key, 0);
	viewer->show();
	QApplication::processEvents();
	auto *view = viewer->findChild<QGraphicsView*>("libraryImageView");
	REQUIRE(view->scene()->items().size() == 1);
	auto *item = dynamic_cast<QGraphicsPixmapItem*>(view->scene()->items().first());
	REQUIRE(item != nullptr);
	REQUIRE(item->pixmap().toImage().pixelColor(20, 20) == QColor("#a46759"));
	REQUIRE(viewer->findChild<QLabel*>("libraryImageStatus")->isVisible());
	REQUIRE(!viewer->findChild<QPushButton*>("libraryImageOpenOriginal")->isEnabled());
	QTest::mouseClick(viewer->findChild<QToolButton*>("libraryFavorite"), Qt::LeftButton);
	REQUIRE(profile->library()->entry(key).favorite);
	QTest::mouseClick(viewer->findChild<QPushButton*>("libraryImageNext"), Qt::LeftButton);
	REQUIRE(viewer->property("libraryImageKey").toString() == key);
	auto *notes = viewer->findChild<QPlainTextEdit*>("libraryImageNotes");
	notes->setFocus();
	QTest::keyClicks(notes, "Relocate this later");
	viewer->reject();
	REQUIRE(profile->library()->entry(key).notes == "Relocate this later");
	QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
	REQUIRE(viewer.isNull());
}
