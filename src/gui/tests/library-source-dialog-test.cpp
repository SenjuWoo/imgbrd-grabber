#include <QApplication>
#include <QAbstractButton>
#include <QComboBox>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPixmap>
#include <QPushButton>
#include <QScopedPointer>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QToolButton>
#include "custom-network-access-manager.h"
#include "image-library-actions.h"
#include "models/image.h"
#include "models/library-importer.h"
#include "models/library-store.h"
#include "models/profile.h"
#include "models/site.h"
#include "viewer/library-source-dialog.h"
#include "catch.h"
#include "source-helpers.h"


TEST_CASE("Source linking validates metadata URLs and keeps candidates explicit", "[foundation][library-source]")
{
	QTemporaryDir directory;
	REQUIRE(directory.isValid());
	const QScopedPointer<Profile> profile(makeLibraryProfile(directory.path()));
	Site *site = profile->getSites().value("danbooru.donmai.us");
	REQUIRE(site != nullptr);
	site->setSetting("sources/usedefault", false, true);
	site->setSetting("sources/source_1", "Json", "Xml");
	site->setSetting("sources/source_2", "", "Json");
	site->setSetting("sources/source_3", "", "Regex");
	site->setSetting("sources/source_4", "", "Rss");
	site->loadConfig();
	QImage pixels(64, 48, QImage::Format_RGB32);
	for (int y = 0; y < pixels.height(); ++y) {
		for (int x = 0; x < pixels.width(); ++x) {
			pixels.setPixelColor(x, y, QColor(x * 4, y * 5, ((x / 8 + y / 8) % 2) * 200));
		}
	}
	const QString path = directory.filePath("import.png");
	REQUIRE(pixels.save(path));
	auto data = LibraryImporter::inspect(path);
	REQUIRE(data.error.isEmpty());
	data.sourceUrls = { "https://danbooru.donmai.us/posts/100", "file:///C:/private.jpg", "javascript:alert(1)", "https://user:secret@example.com/", "http://", "https://danbooru.donmai.us/posts/100" };
	const QString key = profile->library()->saveLocalImage(data);
	REQUIRE(!key.isEmpty());
	const qint64 collection = profile->library()->createCollection("Source review");
	REQUIRE(collection > 0);
	REQUIRE(profile->library()->addToCollection(key, collection));
	REQUIRE(profile->library()->setFavorite(key, true, collection));
	auto remote = QSharedPointer<Image>::create(site, QMap<QString, QString> {
		{ "id", "100" }, { "md5", "11111111111111111111111111111111" },
		{ "file_url", "https://test.invalid/source.png" }, { "page_url", "https://danbooru.donmai.us/posts/100" }
	}, profile.data());
	remote->setPreviewImage(QPixmap::fromImage(pixels));
	REQUIRE(!profile->library()->saveImage(*remote).isEmpty());
	LibrarySourceDialog dialog(profile.data(), key);
	dialog.show();
	auto *urls = dialog.findChild<QListWidget*>("librarySourceUrls");
	REQUIRE(urls != nullptr);
	REQUIRE(urls->count() == 1);
	REQUIRE(!LibrarySourceDialog::isSourceUrl(QUrl("https://user:secret@example.com/")));
	auto *results = dialog.findChild<QListWidget*>("librarySourceCandidates");
	auto *link = dialog.findChild<QPushButton*>("librarySourceLink");
	REQUIRE(results != nullptr);
	REQUIRE(link != nullptr);
	REQUIRE(!link->isEnabled());

	SECTION("Cached visual suggestions require confirmation and preserve scoped preferences")
	{
		ImageLibraryActions existingSearchActions(profile.data(), remote);
		QTest::mouseClick(dialog.findChild<QPushButton*>("librarySourceSimilar"), Qt::LeftButton);
		REQUIRE(QTest::qWaitFor([&]() { return results->count() == 1; }, 5000));
		REQUIRE(results->item(0)->text().contains("Visual candidate"));
		REQUIRE(profile->library()->entry(key).image.value("website").toString().isEmpty());
		results->setCurrentRow(0);
		REQUIRE(link->isEnabled());
		bool confirmed = false;
		QTimer::singleShot(0, [&confirmed]() {
			auto *message = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
			if (message != nullptr) {
				confirmed = true;
				message->button(QMessageBox::Yes)->click();
			}
		});
		QTest::mouseClick(link, Qt::LeftButton);
		REQUIRE(confirmed);
		REQUIRE(dialog.result() == QDialog::Accepted);
		const auto linked = profile->library()->entry(key, collection);
		REQUIRE(linked.favorite);
		REQUIRE(linked.localPaths.contains(path));
		REQUIRE(linked.image.value("website").toString() == site->url());
		REQUIRE(linked.image.value("source_link_evidence").toString().startsWith("User confirmed visual-candidate:"));
		QTest::mouseClick(existingSearchActions.findChild<QToolButton*>("libraryFavorite"), Qt::LeftButton);
		REQUIRE(profile->library()->entry(key).favorite);
		REQUIRE(profile->library()->entries().size() == 1);
		REQUIRE(profile->library()->entry(key, collection).favorite);
	}

	SECTION("Exact lookup rejects mismatched MD5 returned by the real source parser")
	{
		auto *source = dialog.findChild<QComboBox*>("librarySourceSite");
		source->setCurrentIndex(source->findData(site->url()));
		auto *lookup = dialog.findChild<QPushButton*>("librarySourceExact");
		REQUIRE(lookup->isEnabled());
		const QString responsePath = directory.filePath("response.json");
		for (const QString &hash : { QString("22222222222222222222222222222222"), data.md5 }) {
			QFile response(responsePath);
			REQUIRE(response.open(QIODevice::WriteOnly | QIODevice::Truncate));
			const QJsonArray payload { QJsonObject { { "id", 200 }, { "md5", hash }, { "file_url", "https://test.invalid/exact.png" }, { "tag_string", "forest" } } };
			REQUIRE(response.write(QJsonDocument(payload).toJson()) > 0);
			response.close();
			CustomNetworkAccessManager::NextFiles.enqueue(responsePath);
			QTest::mouseClick(lookup, Qt::LeftButton);
			REQUIRE(QTest::qWaitFor([&]() { return lookup->text() == "Find exact match"; }, 5000));
			REQUIRE(results->count() == (hash == data.md5 ? 1 : 0));
			REQUIRE(profile->library()->entry(key).image.value("website").toString().isEmpty());
		}
		REQUIRE(results->item(0)->text().contains("Exact MD5 match"));
		REQUIRE(results->item(0)->icon().isNull()); // Imported pixels are not passed off as source pixels.
	}
}
