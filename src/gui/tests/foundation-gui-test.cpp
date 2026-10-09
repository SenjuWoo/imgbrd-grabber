#include <QApplication>
#include <QFile>
#include <QImageReader>
#include <QPixmap>
#include <QScopedPointer>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QtEndian>
#include <QTimer>
#include <QUrl>
#include <QWidget>
#include "custom-network-access-manager.h"
#include "image-library-actions.h"
#include "models/image.h"
#include "models/profile.h"
#include "models/site.h"
#include "tabs/image-preview.h"
#include "tag-context-menu.h"
#include "ui/QBouton.h"
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
		auto *button = container.findChild<QBouton*>("imagePreviewButton");
		REQUIRE(button != nullptr);
		REQUIRE(!button->icon().isNull());
		REQUIRE(button->toolTip().contains("No thumbnail URL"));
		auto *actions = container.findChild<ImageLibraryActions*>();
		REQUIRE(actions != nullptr);
		REQUIRE(actions->isHidden());
		preview.setChecked(true);
		REQUIRE(!actions->isHidden());
		REQUIRE(container.size() == QSize(156, 156));
		preview.setChecked(false);
		REQUIRE(actions->isHidden());
		preview.abort();
	}
}

TEST_CASE("Cached previews stay image-only and density changes preserve selection and keyboard actions", "[foundation][image-preview]")
{
	QTemporaryDir directory;
	REQUIRE(directory.isValid());
	const QScopedPointer<Profile> profile(makeLibraryProfile(directory.path()));
	Site *site = profile->getSites().value("danbooru.donmai.us");
	REQUIRE(site != nullptr);
	profile->getSettings()->setValue("Gallery/density", 0);
	profile->getSettings()->setValue("borders", 0);
	auto image = QSharedPointer<Image>::create(site, QMap<QString, QString> {
		{ "id", "101" }, { "name", "Cached landscape" }, { "preview_url", "https://test.invalid/cached.png" }
	}, profile.data());
	QPixmap pixels(160, 80);
	pixels.fill(Qt::green);
	image->setPreviewImage(pixels);
	QWidget container;
	ImagePreview preview(image, &container, profile.data(), nullptr, nullptr);
	QSignalSpy finished(&preview, &ImagePreview::finished);
	QSignalSpy opened(&preview, &ImagePreview::clicked);
	preview.load();
	REQUIRE(finished.count() == 1); // Cached pixels do not start a network request.
	auto *button = container.findChild<QBouton*>("imagePreviewButton");
	auto *actions = container.findChild<ImageLibraryActions*>();
	REQUIRE(button != nullptr);
	REQUIRE(actions != nullptr);
	REQUIRE(actions->isHidden());
	REQUIRE(container.size() == QSize(128, 128));
	REQUIRE(button->iconSize() == QSize(128, 64));
	preview.setChecked(true);
	const QSize size = container.size();
	REQUIRE(!actions->isHidden());
	REQUIRE(container.size() == size);
	QTest::keyClick(button, Qt::Key_Space);
	REQUIRE(!button->isChecked());
	REQUIRE(actions->isHidden());
	QTest::keyClick(button, Qt::Key_Return);
	REQUIRE(opened.count() == 1);
	profile->getSettings()->setValue("Gallery/density", 2);
	preview.refreshDensity();
	REQUIRE(container.size() == QSize(256, 256));
	REQUIRE(button->iconSize() == QSize(256, 128));
	REQUIRE(finished.count() == 1);
	REQUIRE(actions->geometry().bottom() == container.height() - 1);
	REQUIRE(button->accessibleName() == "Cached landscape");

	// Logical icon dimensions are independent of the monitor's pixel ratio.
	pixels.setDevicePixelRatio(2.0);
	button->scale(pixels, QSize(180, 180));
	REQUIRE(button->iconSize() == QSize(180, 90));
	REQUIRE(button->icon().pixmap(QSize(180, 90), QIcon::Selected).toImage().pixelColor(10, 10) == QColor(Qt::green));
}

TEST_CASE("Remote previews use real alternate URLs and reject oversized or undecodable images", "[foundation][image-preview]")
{
	QTemporaryDir directory;
	REQUIRE(directory.isValid());
	const QScopedPointer<Profile> profile(makeLibraryProfile(directory.path()));
	Site *site = profile->getSites().value("danbooru.donmai.us");
	REQUIRE(site != nullptr);
	profile->getSettings()->setValue("thumbnailSmartSize", false);
	QMap<QString, QString> details { { "id", "102" }, { "preview_url", "https://test.invalid/preview.webp" } };
	const QString path = directory.filePath("reply.bin");

	SECTION("A failed thumbnail falls back to the actual sample URL")
	{
		details.insert("sample_url", "https://test.invalid/sample.png");
		QPixmap pixels(64, 32);
		pixels.fill(Qt::blue);
		REQUIRE(pixels.save(path, "PNG"));
		CustomNetworkAccessManager::NextFiles.enqueue("404");
		CustomNetworkAccessManager::NextFiles.enqueue(path);
	}
	SECTION("A declared image dimension is rejected before decoding pixels")
	{
		QImage pixels(8193, 1, QImage::Format_RGB32);
		pixels.fill(Qt::red);
		REQUIRE(pixels.save(path, "PNG"));
	}
	SECTION("The pixel count is bounded before decoding")
	{
		QByteArray header(54, '\0');
		header[0] = 'B';
		header[1] = 'M';
		qToLittleEndian<quint32>(54, reinterpret_cast<uchar*>(header.data() + 2));
		qToLittleEndian<quint32>(54, reinterpret_cast<uchar*>(header.data() + 10));
		qToLittleEndian<quint32>(40, reinterpret_cast<uchar*>(header.data() + 14));
		qToLittleEndian<quint32>(4097, reinterpret_cast<uchar*>(header.data() + 18));
		qToLittleEndian<quint32>(4096, reinterpret_cast<uchar*>(header.data() + 22));
		qToLittleEndian<quint16>(1, reinterpret_cast<uchar*>(header.data() + 26));
		qToLittleEndian<quint16>(24, reinterpret_cast<uchar*>(header.data() + 28));
		QFile response(path);
		REQUIRE(response.open(QIODevice::WriteOnly));
		REQUIRE(response.write(header) == header.size());
		response.close();
		QImageReader reader(path);
		REQUIRE(reader.size() == QSize(4097, 4096));
	}
	SECTION("A response larger than 16 MiB is rejected")
	{
		QFile response(path);
		REQUIRE(response.open(QIODevice::WriteOnly));
		REQUIRE(response.resize(16 * 1024 * 1024 + 1));
	}
	SECTION("HTML is not treated as a missing image")
	{
		QFile response(path);
		REQUIRE(response.open(QIODevice::WriteOnly));
		REQUIRE(response.write("<html>Login required</html>") > 0);
	}
	if (!details.contains("sample_url")) {
		CustomNetworkAccessManager::NextFiles.enqueue(path);
	}
	auto image = QSharedPointer<Image>::create(site, details, profile.data());
	QWidget container;
	ImagePreview preview(image, &container, profile.data(), nullptr, nullptr);
	QSignalSpy finished(&preview, &ImagePreview::finished);
	preview.load();
	REQUIRE(QTest::qWaitFor([&]() { return finished.count() == 1; }, 5000));
	REQUIRE(CustomNetworkAccessManager::NextFiles.isEmpty());
	auto *button = container.findChild<QBouton*>("imagePreviewButton");
	REQUIRE(button != nullptr);
	REQUIRE(!button->icon().isNull());
	if (details.contains("sample_url")) {
		REQUIRE(!image->previewImage().isNull());
		REQUIRE(image->previewImage().toImage().pixelColor(10, 10) == QColor(Qt::blue));
		REQUIRE(!button->toolTip().contains("could not load"));
	} else {
		REQUIRE(image->previewImage().isNull());
		REQUIRE((button->toolTip().contains("limit") || button->toolTip().contains("dimensions")));
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
