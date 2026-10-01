#include <QApplication>
#include <QPixmap>
#include <QScopedPointer>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>
#include <QWidget>
#include "models/image.h"
#include "models/profile.h"
#include "models/site.h"
#include "tabs/image-preview.h"
#include "tag-context-menu.h"
#include "viewer/details-window.h"
#include "viewer/viewer-window.h"
#include "catch.h"
#include "source-helpers.h"


TEST_CASE("Missing thumbnails can be canceled and destroyed without a network reply", "[foundation][image-preview]")
{
	QTemporaryDir directory;
	REQUIRE(directory.isValid());
	const QScopedPointer<Profile> profile(makeLibraryProfile(directory.path()));
	Site *site = profile->getSites().value("danbooru.donmai.us");
	REQUIRE(site != nullptr);
	auto image = QSharedPointer<Image>::create(site, QMap<QString, QString> { { "id", "100" } }, profile.data());
	{
		QWidget container;
		ImagePreview preview(image, &container, profile.data(), nullptr, nullptr);
		preview.abort(); // Search tabs can be closed before thumbnail loading starts.
	}
	{
		QWidget container;
		ImagePreview preview(image, &container, profile.data(), nullptr, nullptr);
		QSignalSpy finished(&preview, &ImagePreview::finished);
		preview.load(); // No thumbnail URL means no NetworkReply is created.
		REQUIRE(finished.count() == 1);
		preview.abort();
	}
}

TEST_CASE("Viewer navigation applies tag actions to the displayed picture source", "[foundation][viewer]")
{
	QTemporaryDir directory;
	REQUIRE(directory.isValid());
	const QScopedPointer<Profile> profile(makeLibraryProfile(directory.path()));
	Site *firstSite = profile->getSites().value("danbooru.donmai.us");
	Site *secondSite = profile->getSites().value("hijiribe.donmai.us");
	REQUIRE(firstSite != nullptr);
	REQUIRE(secondSite != nullptr);
	QPixmap pixels(64, 64);
	pixels.fill(Qt::green);
	const QString path = directory.filePath("fixture.png");
	REQUIRE(pixels.save(path));
	profile->getSettings()->setValue("Save/path", directory.path());
	profile->getSettings()->setValue("Save/filename", "fixture.png");
	profile->getSettings()->setValue("Viewer/useVideoPlayer", false);
	profile->getSettings()->setValue("preload", 0);
	const QMap<QString, QString> details { { "id", "100" }, { "file_url", QUrl::fromLocalFile(path).toString() } };
	auto first = QSharedPointer<Image>::create(firstSite, details, profile.data());
	auto second = QSharedPointer<Image>::create(secondSite, details, profile.data());
	first->setPreviewImage(pixels);
	second->setPreviewImage(pixels);
	ViewerWindow viewer({ first, second }, first, firstSite, profile.data(), nullptr, nullptr);
	REQUIRE(viewer.findChildren<DetailsWindow*>().size() == 1);
	viewer.next();
	viewer.next();
	viewer.previous();
	REQUIRE(viewer.findChildren<DetailsWindow*>().size() == 1);
	viewer.linkHovered("foundation_tag");
	bool invoked = false;
	QTimer::singleShot(0, &viewer, [&viewer, &invoked]() {
		auto *menu = viewer.findChild<TagContextMenu*>();
		if (menu != nullptr) {
			invoked = QMetaObject::invokeMethod(menu, "favorite", Qt::DirectConnection);
			menu->close();
		}
	});
	viewer.contextMenu(QPoint());
	// Offscreen macOS menus may return before dispatching the queued action.
	QApplication::processEvents();
	REQUIRE(invoked);
	const int index = profile->getFavorites().indexOf(Favorite("foundation_tag"));
	REQUIRE(index >= 0);
	REQUIRE(profile->getFavorites()[index].getSites() == QList<Site*> { secondSite });
}
