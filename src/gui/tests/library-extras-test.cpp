#include <QApplication>
#include <QDialog>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QScopeGuard>
#include <QSettings>
#include <QScopedPointer>
#include <QTemporaryDir>
#include <QTest>
#include "custom-network-access-manager.h"
#include "models/image.h"
#include "models/library-health.h"
#include "models/library-importer.h"
#include "models/library-store.h"
#include "models/profile.h"
#include "models/site.h"
#include "tabs/library-tab.h"
#include "tabs/tag-tab.h"
#include "viewer/library-batch-source-dialog.h"
#include "viewer/library-health-dialog.h"
#include "catch.h"
#include "source-helpers.h"

namespace
{
	class MirrorFixture : public TagTab
	{
		public:
			using TagTab::TagTab;
			using SearchTab::loadSites;
	};

	QString savePicture(const QString &path, const QColor &color)
	{
		QImage pixels(64, 48, QImage::Format_RGB32);
		pixels.fill(color);
		REQUIRE(pixels.save(path));
		return path;
	}
}

TEST_CASE("A folder imported as loved is liked, and only what was unrated", "[library][import][loved]")
{
	QTemporaryDir directory;
	QTemporaryDir files;
	const QScopedPointer<Profile> profile(makeLibraryProfile(directory.path()));
	savePicture(files.filePath("a.png"), Qt::red);
	savePicture(files.filePath("b.png"), Qt::blue);
	const QString favoriteKey = profile->library()->saveLocalImage(LibraryImporter::inspect(files.filePath("b.png")));
	REQUIRE(profile->library()->setFavorite(favoriteKey, true));
	LibraryTab library(profile.data(), nullptr);
	library.show();
	QApplication::processEvents();
	library.importPaths({files.path()}, false, {}, true);
	REQUIRE(QTest::qWaitFor([&]() { return !library.importing(); }, 15000));
	const auto entries = profile->library()->entries();
	REQUIRE(entries.size() == 2);
	for (const auto &entry : entries) {
		REQUIRE((entry.liked || entry.favorite));
	}
	REQUIRE(profile->library()->entry(favoriteKey).favorite); // kept as it was
	REQUIRE_FALSE(profile->library()->entry(favoriteKey).liked);

	// A plain import never rates anything.
	savePicture(files.filePath("c.png"), Qt::green);
	library.importPaths({files.filePath("c.png")});
	REQUIRE(QTest::qWaitFor([&]() { return !library.importing(); }, 15000));
	int unrated = 0;
	for (const auto &entry : profile->library()->entries()) {
		unrated += entry.liked || entry.favorite ? 0 : 1;
	}
	REQUIRE(unrated == 1);
}

TEST_CASE("Batch source lookup proposes exact MD5 matches and links only what is checked", "[library][library-source][batch]")
{
	const auto cleanup = qScopeGuard([]() { CustomNetworkAccessManager::NextFiles.clear(); });
	QTemporaryDir directory;
	const QScopedPointer<Profile> profile(makeLibraryProfile(directory.path()));
	Site *site = profile->getSites().value("danbooru.donmai.us");
	REQUIRE(site != nullptr);
	site->setSetting("sources/usedefault", false, true);
	site->setSetting("sources/source_1", "Json", "Xml");
	site->setSetting("sources/source_2", "", "Json");
	site->setSetting("sources/source_3", "", "Regex");
	site->setSetting("sources/source_4", "", "Rss");
	site->loadConfig();
	profile->getSettings()->setValue("sites", QStringList {site->url()});
	const auto data = LibraryImporter::inspect(savePicture(directory.filePath("plain.png"), Qt::yellow));
	REQUIRE(data.error.isEmpty());
	const QString key = profile->library()->saveLocalImage(data);
	REQUIRE(!key.isEmpty());
	REQUIRE(profile->library()->entry(key).tags().isEmpty());

	LibraryBatchSourceDialog dialog(profile.data());
	dialog.show();
	REQUIRE(dialog.pendingCount() == 1);
	auto *start = dialog.findChild<QPushButton*>("libraryBatchStart");
	auto *list = dialog.findChild<QListWidget*>("libraryBatchMatches");
	auto *link = dialog.findChild<QPushButton*>("libraryBatchLink");
	REQUIRE(start->isEnabled());
	REQUIRE_FALSE(link->isEnabled());

	const QString response = directory.filePath("response.json");
	QFile file(response);
	REQUIRE(file.open(QIODevice::WriteOnly));
	file.write(QJsonDocument(QJsonArray {QJsonObject {{"id", 300}, {"md5", data.md5}, {"file_url", "https://test.invalid/found.png"}, {"tag_string", "forest smile"}}}).toJson());
	file.close();
	CustomNetworkAccessManager::NextFiles.enqueue(response);
	QTest::mouseClick(start, Qt::LeftButton);
	REQUIRE(QTest::qWaitFor([&]() { return list->count() == 1 && start->text() == "Start lookup"; }, 10000));
	REQUIRE(list->item(0)->checkState() == Qt::Checked);
	REQUIRE(profile->library()->entry(key).image.value("website").toString().isEmpty()); // nothing linked yet

	list->item(0)->setCheckState(Qt::Unchecked);
	REQUIRE_FALSE(link->isEnabled());
	list->item(0)->setCheckState(Qt::Checked);
	QTest::mouseClick(link, Qt::LeftButton);
	REQUIRE(profile->library()->entry(key).image.value("website").toString() == site->url());
	REQUIRE(profile->library()->entry(key).tags().contains("forest"));
	REQUIRE(list->count() == 0);
}

TEST_CASE("Library health reports what is wrong and removes only catalog entries", "[library][health]")
{
	QTemporaryDir directory;
	const QScopedPointer<Profile> profile(makeLibraryProfile(directory.path()));
	const QString path = savePicture(directory.filePath("here.png"), Qt::cyan);
	const QString gone = savePicture(directory.filePath("gone.png"), Qt::magenta);
	const QString goneKey = profile->library()->saveLocalImage(LibraryImporter::inspect(gone));
	const QString hereKey = profile->library()->saveLocalImage(LibraryImporter::inspect(path));
	REQUIRE(profile->library()->setLiked(goneKey, true));
	REQUIRE(profile->library()->setLiked(hereKey, true));
	REQUIRE(QFile::remove(gone));

	LibraryHealthDialog dialog(profile.data());
	dialog.show();
	const auto *missing = dialog.health().issue("missing-files");
	REQUIRE(missing != nullptr);
	REQUIRE(missing->keys == QStringList {goneKey});
	REQUIRE(dialog.health().issue("no-tags") != nullptr); // both imports carry no tags
	auto *issues = dialog.findChild<QListWidget*>("libraryHealthIssues");
	REQUIRE(issues->count() == dialog.health().issues.size());
	REQUIRE(profile->library()->entries().size() == 2);
}

TEST_CASE("Mirrors of one server are searched once unless the user turns that off", "[search][site-mirrors]")
{
	QTemporaryDir directory;
	const QScopedPointer<Profile> profile(makeLibraryProfile(directory.path()));
	auto *main = profile->getSites().value("danbooru.donmai.us");
	auto *mirror = profile->getSites().value("hijiribe.donmai.us");
	REQUIRE(main != nullptr);
	REQUIRE(mirror != nullptr);
	MirrorFixture tab(profile.data(), nullptr, nullptr);
	tab.setSources({main, mirror});
	REQUIRE(tab.loadSites() == QList<Site*> {main});
	profile->getSettings()->setValue("skipMirrors", false);
	REQUIRE(tab.loadSites().size() == 2);
}

TEST_CASE("A rate-limited source tells the user it is waiting instead of looking hung", "[search][rate-limit]")
{
	const auto cleanup = qScopeGuard([]() { CustomNetworkAccessManager::NextFiles.clear(); });
	QTemporaryDir directory;
	const QScopedPointer<Profile> profile(makeLibraryProfile(directory.path()));
	Site *site = profile->getSites().value("danbooru.donmai.us");
	REQUIRE(site != nullptr);
	site->setSetting("sources/usedefault", false, true);
	site->setSetting("sources/source_1", "Json", "Xml");
	site->setSetting("sources/source_2", "", "Json");
	site->setSetting("sources/source_3", "", "Regex");
	site->setSetting("sources/source_4", "", "Rss");
	site->setSetting("download/throttle_retry", 1, 60);
	site->setSetting("download/throttle_page", 0, 1);
	site->loadConfig();
	profile->getSettings()->setValue("useregexfortags", false);
	int limited = 0;
	QObject::connect(site, &Site::rateLimited, [&limited](Site *, int seconds) { limited += seconds; });
	const QString response = directory.filePath("response.json");
	QFile file(response);
	REQUIRE(file.open(QIODevice::WriteOnly));
	file.write(QJsonDocument(QJsonArray {QJsonObject {{"id", 1}, {"md5", "11111111111111111111111111111111"}, {"file_url", "https://test.invalid/1.png"}, {"tag_string", "forest"}}}).toJson());
	file.close();
	CustomNetworkAccessManager::NextFiles.enqueue("429");
	CustomNetworkAccessManager::NextFiles.enqueue(response);
	MirrorFixture tab(profile.data(), nullptr, nullptr);
	tab.show();
	tab.setSources({site});
	tab.setTags("forest");
	REQUIRE(QTest::qWaitFor([&]() { return limited > 0; }, 10000));
	REQUIRE(limited == 1); // one notice, carrying the retry delay
	bool toast = false;
	for (const auto *label : tab.findChildren<QLabel*>()) {
		toast = toast || label->text().contains("rate-limiting");
	}
	REQUIRE(toast);
}
