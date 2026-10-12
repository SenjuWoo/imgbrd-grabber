#include <QApplication>
#include <QAbstractButton>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonObject>
#include <QScopeGuard>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPixmap>
#include <QPushButton>
#include <QSettings>
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

		// The default asks each of the selected sources in turn.
		profile->getSettings()->setValue("sites", QStringList { "missing.invalid", site->url() });
		source->setCurrentIndex(0);
		REQUIRE(source->currentData().toString() == "*");
		CustomNetworkAccessManager::NextFiles.enqueue(responsePath);
		QTest::mouseClick(lookup, Qt::LeftButton);
		REQUIRE(QTest::qWaitFor([&]() { return lookup->text() == "Find exact match"; }, 5000));
		REQUIRE(results->count() == 1);
	}
}

TEST_CASE("Source linking finds a Pixiv artwork from the file name and compares its original file", "[foundation][library-source][pixiv]")
{
	auto clearReplies = qScopeGuard([]() { CustomNetworkAccessManager::NextFiles.clear(); });
	QTemporaryDir directory;
	REQUIRE(directory.isValid());
	const QString sources = QDir::cleanPath(QFileInfo(QString::fromUtf8(__FILE__)).absolutePath() + "/../../sites/Pixiv");
	REQUIRE(QDir().mkpath(directory.filePath("sites/Pixiv/www.pixiv.net")));
	for (const QString &file : {QString("model.js"), QString("sites.txt"), QString("www.pixiv.net/defaults.ini")}) {
		REQUIRE(QFile::copy(sources + "/" + file, directory.filePath("sites/Pixiv/" + file)));
	}
	const QScopedPointer<Profile> profile(makeLibraryProfile(directory.path()));
	REQUIRE(profile->getSites().contains("www.pixiv.net"));
	QImage pixels(64, 48, QImage::Format_RGB32);
	pixels.fill(QColor(200, 120, 60));
	const QString path = directory.filePath("149621459_p5.png");
	REQUIRE(pixels.save(path));
	const auto data = LibraryImporter::inspect(path);
	REQUIRE(data.error.isEmpty());
	const QString key = profile->library()->saveLocalImage(data);
	REQUIRE(!key.isEmpty());

	LibrarySourceDialog dialog(profile.data(), key);
	dialog.show();
	auto *pixiv = dialog.findChild<QPushButton*>("librarySourcePixiv");
	REQUIRE(pixiv->isVisible());
	REQUIRE(pixiv->text().contains("149621459"));
	REQUIRE(pixiv->text().contains("page 6"));

	// Pixiv's public artwork answer (file URLs hidden, as for restricted artworks), a wrong extension guess, then the original.
	const QJsonObject details {{"error", false}, {"body", QJsonObject {
		{"illustId", "149621459"}, {"illustTitle", "Title"}, {"userName", "Some Artist"}, {"pageCount", 13},
		{"tags", QJsonObject {{"tags", QJsonArray {QJsonObject {{"tag", "hairy_slut"}}}}}},
		{"userIllusts", QJsonObject {{"149621459", QJsonObject {{"url", "https://i.pximg.net/c/250x250_80_a2/custom-thumb/img/2026/09/13/17/32/13/149621459_p0_custom1200.jpg"}}}}},
	}}};
	QFile response(directory.filePath("details.json"));
	REQUIRE(response.open(QIODevice::WriteOnly));
	REQUIRE(response.write(QJsonDocument(details).toJson()) > 0);
	response.close();
	CustomNetworkAccessManager::NextFiles.enqueue(response.fileName());
	CustomNetworkAccessManager::NextFiles.enqueue("404");
	CustomNetworkAccessManager::NextFiles.enqueue(path);

	auto *results = dialog.findChild<QListWidget*>("librarySourceCandidates");
	QTest::mouseClick(pixiv, Qt::LeftButton);
	REQUIRE(QTest::qWaitFor([&]() { return results->count() == 1; }, 5000));
	REQUIRE(results->item(0)->text().contains("Exact file match on Pixiv"));
	REQUIRE(results->item(0)->text().contains("www.pixiv.net"));
	results->setCurrentRow(0);
	bool confirmed = false;
	QTimer::singleShot(0, [&confirmed]() {
		if (auto *message = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
			confirmed = true;
			message->button(QMessageBox::Yes)->click();
		}
	});
	QTest::mouseClick(dialog.findChild<QPushButton*>("librarySourceLink"), Qt::LeftButton);
	REQUIRE(confirmed);
	const auto linked = profile->library()->entry(key);
	REQUIRE(linked.image.value("website").toString() == "www.pixiv.net");
	REQUIRE(linked.image.value("page_url").toString() == "https://www.pixiv.net/artworks/149621459");
	REQUIRE(linked.image.value("source_link_evidence").toString().startsWith("User confirmed exact-md5:"));
	REQUIRE(linked.tags().contains("Some_Artist"));
	REQUIRE(linked.tags().contains("hairy_slut"));
	REQUIRE(linked.localPaths.contains(path));
}
