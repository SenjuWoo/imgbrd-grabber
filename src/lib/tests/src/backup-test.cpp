#include <QDir>
#include <QFile>
#include <QSettings>
#include <QImage>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include "catch.h"
#include "backup.h"
#include "models/profile.h"
#include "models/library-store.h"
#include "models/library-importer.h"
#include "utils/file-utils.h"
#include "source-helpers.h"
#include "monitoring/monitor-manager.h"
#include "utils/zip.h"


TEST_CASE("Backup")
{
	setupSource("Danbooru (2.0)");
	setupSite("Danbooru (2.0)", "danbooru.donmai.us");
	const QScopedPointer<Profile> profile(makeProfile());
	Site *site = profile->getSites().value("danbooru.donmai.us");
	QTemporaryDir tmpDir;

	const QString zipFile = tmpDir.filePath("backup-test.zip");
	const QString zipDir = tmpDir.filePath("unpacked");
	REQUIRE(QDir().mkpath(zipDir));

	SECTION("settings.ini")
	{
		// Set a single setting on the profile and back it up
		profile->getSettings()->setValue("foo", "bar");
		REQUIRE(saveBackup(profile.data(), zipFile));
		REQUIRE(QFile::exists(zipFile));

		// Unzipping the backup should contain a settings.ini file
		REQUIRE(unzipFile(zipFile, zipDir));
		const QStringList files = QDir(zipDir).entryList(QDir::Files | QDir::NoDotAndDotDot);
		REQUIRE(files.contains("settings.ini"));

		// Creating a profile file from the backup directory should have the previous setting
		Profile after(zipDir);
		REQUIRE(after.getSettings()->childKeys() == QStringList() << "foo");
		REQUIRE(after.getSettings()->value("foo").toString() == "bar");

		// Create a fresh profile and import the backup in it, the setting key should be available
		const QScopedPointer<Profile> fresh(makeProfile());
		fresh->getSettings()->setValue("key", "value"); // This should be overwritten by the backup
		REQUIRE(fresh->getSettings()->childKeys() == QStringList() << "key");
		REQUIRE(fresh->getSettings()->value("key").toString() == "value");
		REQUIRE(loadBackup(fresh.data(), zipFile));
		REQUIRE(fresh->getSettings()->childKeys() == QStringList() << "foo");
		REQUIRE(fresh->getSettings()->value("foo").toString() == "bar");
	}

	SECTION("favorites.json")
	{
		// Set a single setting on the profile and back it up
		profile->addFavorite(Favorite("test_tag"));
		REQUIRE(saveBackup(profile.data(), zipFile));
		REQUIRE(QFile::exists(zipFile));

		// Unzipping the backup should contain a favorites.json file
		REQUIRE(unzipFile(zipFile, zipDir));
		const QStringList files = QDir(zipDir).entryList(QDir::Files | QDir::NoDotAndDotDot);
		REQUIRE(files.contains("favorites.json"));

		// Creating a profile file from the backup directory should have the previous setting
		Profile after(zipDir);
		REQUIRE(after.getFavorites().count() == 1);
		REQUIRE(after.getFavorites()[0].getName() == "test_tag");

		// Create a fresh profile and import the backup in it, the setting key should be available
		const QScopedPointer<Profile> fresh(makeProfile());
		fresh->addFavorite(Favorite("another_tag")); // This should be overwritten by the backup
		REQUIRE(fresh->getFavorites().count() == 1);
		REQUIRE(loadBackup(fresh.data(), zipFile));
		REQUIRE(fresh->getFavorites().count() == 1);
		REQUIRE(fresh->getFavorites()[0].getName() == "test_tag");
	}

	SECTION("viewitlater.txt")
	{
		// Set a single setting on the profile and back it up
		profile->addKeptForLater("test_tag");
		REQUIRE(saveBackup(profile.data(), zipFile));
		REQUIRE(QFile::exists(zipFile));

		// Unzipping the backup should contain a viewitlater.txt file
		REQUIRE(unzipFile(zipFile, zipDir));
		const QStringList files = QDir(zipDir).entryList(QDir::Files | QDir::NoDotAndDotDot);
		REQUIRE(files.contains("viewitlater.txt"));

		// Creating a profile file from the backup directory should have the previous setting
		Profile after(zipDir);
		REQUIRE(after.getKeptForLater() == QStringList() << "test_tag");

		// Create a fresh profile and import the backup in it, the setting key should be available
		const QScopedPointer<Profile> fresh(makeProfile());
		fresh->addKeptForLater("another_tag"); // This should be overwritten by the backup
		REQUIRE(loadBackup(fresh.data(), zipFile));
		REQUIRE(fresh->getKeptForLater() == QStringList() << "test_tag");
	}

	SECTION("ignore.txt")
	{
		// Set a single setting on the profile and back it up
		profile->addIgnored("test_tag");
		REQUIRE(saveBackup(profile.data(), zipFile));
		REQUIRE(QFile::exists(zipFile));

		// Unzipping the backup should contain a ignore.txt file
		REQUIRE(unzipFile(zipFile, zipDir));
		const QStringList files = QDir(zipDir).entryList(QDir::Files | QDir::NoDotAndDotDot);
		REQUIRE(files.contains("ignore.txt"));

		// Creating a profile file from the backup directory should have the previous setting
		Profile after(zipDir);
		REQUIRE(after.getIgnored() == QStringList() << "test_tag");

		// Create a fresh profile and import the backup in it, the setting key should be available
		const QScopedPointer<Profile> fresh(makeProfile());
		fresh->addIgnored("another_tag"); // This should be overwritten by the backup
		REQUIRE(loadBackup(fresh.data(), zipFile));
		REQUIRE(fresh->getIgnored() == QStringList() << "test_tag");
	}

	SECTION("wordsc.txt")
	{
		// Set a single setting on the profile and back it up
		profile->addAutoComplete("test_tag");
		REQUIRE(saveBackup(profile.data(), zipFile));
		REQUIRE(QFile::exists(zipFile));

		// Unzipping the backup should contain a wordsc.txt file
		REQUIRE(unzipFile(zipFile, zipDir));
		const QStringList files = QDir(zipDir).entryList(QDir::Files | QDir::NoDotAndDotDot);
		REQUIRE(files.contains("wordsc.txt"));

		// Creating a profile file from the backup directory should have the previous setting
		Profile after(zipDir);
		REQUIRE(after.getAutoComplete().contains("test_tag"));

		// Create a fresh profile and import the backup in it, the setting key should be available
		const QScopedPointer<Profile> fresh(makeProfile());
		fresh->addAutoComplete("another_tag"); // This should be overwritten by the backup
		REQUIRE(loadBackup(fresh.data(), zipFile));
		REQUIRE(fresh->getAutoComplete().contains("test_tag"));
		REQUIRE(!fresh->getAutoComplete().contains("another_tag"));
	}

	SECTION("blacklist.txt")
	{
		// Set a single setting on the profile and back it up
		profile->addBlacklistedTag("test_tag");
		REQUIRE(saveBackup(profile.data(), zipFile));
		REQUIRE(QFile::exists(zipFile));

		// Unzipping the backup should contain a blacklist.txt file
		REQUIRE(unzipFile(zipFile, zipDir));
		const QStringList files = QDir(zipDir).entryList(QDir::Files | QDir::NoDotAndDotDot);
		REQUIRE(files.contains("blacklist.txt"));

		// Creating a profile file from the backup directory should have the previous setting
		Profile after(zipDir);
		REQUIRE(after.getBlacklist().contains("test_tag"));

		// Create a fresh profile and import the backup in it, the setting key should be available
		const QScopedPointer<Profile> fresh(makeProfile());
		fresh->addBlacklistedTag("another_tag"); // This should be overwritten by the backup
		REQUIRE(loadBackup(fresh.data(), zipFile));
		REQUIRE(fresh->getBlacklist().contains("test_tag"));
		REQUIRE(!fresh->getBlacklist().contains("another_tag"));
	}

	SECTION("site-specific blacklist.txt")
	{
		profile->addBlacklistedTags({ "website:danbooru.donmai.us", "test_tag" });
		REQUIRE(profile->getBlacklist().toString() == "website:danbooru.donmai.us test_tag");
		REQUIRE(saveBackup(profile.data(), zipFile));

		REQUIRE(unzipFile(zipFile, zipDir));
		Profile after(zipDir);
		REQUIRE(after.getBlacklist().toString() == "website:danbooru.donmai.us test_tag");
	}

	SECTION("monitors.json")
	{
		// Set a single setting on the profile and back it up
		profile->monitorManager()->add(Monitor(profile->getSettings(), {site}, QStringList() << "test_tag"));
		REQUIRE(saveBackup(profile.data(), zipFile));
		REQUIRE(QFile::exists(zipFile));

		// Unzipping the backup should contain a monitors.json file
		REQUIRE(unzipFile(zipFile, zipDir));
		const QStringList files = QDir(zipDir).entryList(QDir::Files | QDir::NoDotAndDotDot);
		REQUIRE(files.contains("monitors.json"));

		// Creating a profile file from the backup directory should have the previous setting
		Profile after(zipDir);
		REQUIRE(after.monitorManager()->monitors().count() == 1);
		REQUIRE(after.monitorManager()->monitors()[0].query().tags == QStringList() << "test_tag");

		// Create a fresh profile and import the backup in it, the setting key should be available
		const QScopedPointer<Profile> fresh(makeProfile());
		fresh->monitorManager()->add(Monitor(profile->getSettings(), {site}, QStringList() << "another_tag")); // This should be overwritten by the backup
		REQUIRE(loadBackup(fresh.data(), zipFile));
		REQUIRE(fresh->monitorManager()->monitors().count() == 1);
		REQUIRE(fresh->monitorManager()->monitors()[0].query().tags == QStringList() << "test_tag");
	}

	SECTION("history.json")
	{
		// Set a single setting on the profile and back it up
		profile->getHistory()->addQuery(QStringList() << "test_tag", {site});
		REQUIRE(saveBackup(profile.data(), zipFile));
		REQUIRE(QFile::exists(zipFile));

		// Unzipping the backup should contain a history.json file
		REQUIRE(unzipFile(zipFile, zipDir));
		const QStringList files = QDir(zipDir).entryList(QDir::Files | QDir::NoDotAndDotDot);
		REQUIRE(files.contains("history.json"));

		// Creating a profile file from the backup directory should have the previous setting
		Profile after(zipDir);
		REQUIRE(after.getHistory()->entries().count() == 1);
		REQUIRE(after.getHistory()->entries()[0]->query.tags == QStringList() << "test_tag");

		// Create a fresh profile and import the backup in it, the setting key should be available
		const QScopedPointer<Profile> fresh(makeProfile());
		fresh->getHistory()->addQuery(QStringList() << "another_tag", {site}); // This should be overwritten by the backup
		REQUIRE(loadBackup(fresh.data(), zipFile));
		REQUIRE(fresh->getHistory()->entries().count() == 1);
		REQUIRE(fresh->getHistory()->entries()[0]->query.tags == QStringList() << "test_tag");
	}

	SECTION("md5s.sqlite")
	{
		// Set a single setting on the profile and back it up
		profile->addMd5("098f6bcd4621d373cade4e832627b4f6", "tests/resources/image_1x1.png");
		REQUIRE(saveBackup(profile.data(), zipFile));
		REQUIRE(QFile::exists(zipFile));

		// Unzipping the backup should contain a md5s.sqlite file
		REQUIRE(unzipFile(zipFile, zipDir));
		const QStringList files = QDir(zipDir).entryList(QDir::Files | QDir::NoDotAndDotDot);
		REQUIRE(files.contains("md5s.sqlite"));

		// Creating a profile file from the backup directory should have the previous setting
		Profile after(zipDir);
		REQUIRE(!after.md5Exists("098f6bcd4621d373cade4e832627b4f6").isEmpty());

		// Create a fresh profile and import the backup in it, the setting key should be available
		const QScopedPointer<Profile> fresh(makeProfile());
		fresh->addMd5("b32d73e56ec99bc5ec8f83871cde708a", "tests/resources/image_200x200.png");
		REQUIRE(loadBackup(fresh.data(), zipFile));
		REQUIRE(!fresh->md5Exists("098f6bcd4621d373cade4e832627b4f6").isEmpty());
		REQUIRE(fresh->md5Exists("b32d73e56ec99bc5ec8f83871cde708a").isEmpty());
	}
}


TEST_CASE("Backups preserve the portable Library and restore its live store", "[library][backup]")
{
	QTemporaryDir sourceDirectory, destinationDirectory;
	REQUIRE(sourceDirectory.isValid());
	REQUIRE(destinationDirectory.isValid());
	const QScopedPointer<Profile> source(makeLibraryProfile(sourceDirectory.path()));
	QImage picture(16, 12, QImage::Format_RGB32);
	picture.fill(Qt::green);
	const QString original = sourceDirectory.filePath("original.png");
	REQUIRE(picture.save(original, "PNG"));
	const auto imported = LibraryImporter::inspect(original, sourceDirectory.filePath("library-media"));
	REQUIRE(imported.error.isEmpty());
	auto *sourceStore = source->library();
	const qint64 collection = sourceStore->createCollection("Portable collection");
	REQUIRE(collection > 0);
	const QString key = sourceStore->saveLocalImage(imported, collection);
	REQUIRE(!key.isEmpty());
	REQUIRE(sourceStore->setFavorite(key, true, collection));
	REQUIRE(sourceStore->setLiked(key, true));
	REQUIRE(sourceStore->setNotes(key, "scoped note", collection));
	REQUIRE(sourceStore->setCollectionCover(collection, key));
	const QString archive = sourceDirectory.filePath("library-backup.zip");
	REQUIRE(saveBackup(source.data(), archive));
	const QString extracted = sourceDirectory.filePath("extracted");
	REQUIRE(unzipFile(archive, extracted));
	{
		LibraryStore snapshot(QDir(extracted).filePath("library.sqlite"));
		REQUIRE(snapshot.isReady());
		REQUIRE(snapshot.entry(key).liked);
		REQUIRE(snapshot.entry(key, collection).favorite);
		REQUIRE(QFile::exists(snapshot.entry(key).localPaths.first()));
	}
	const QScopedPointer<Profile> destination(makeLibraryProfile(destinationDirectory.path()));
	auto *liveStore = destination->library();
	REQUIRE(liveStore->createCollection("Old collection") > 0);
	REQUIRE(loadBackup(destination.data(), archive));
	REQUIRE(destination->library() == liveStore);
	REQUIRE(liveStore->entry(key).liked);
	REQUIRE(liveStore->entry(key, collection).favorite);
	REQUIRE(liveStore->entry(key, collection).notes == "scoped note");
	REQUIRE(liveStore->collections().size() == 1);
	REQUIRE(liveStore->collections().first().name == "Portable collection");
	REQUIRE(!liveStore->collections().first().cover.isEmpty());
	const QString restoredFile = liveStore->entry(key).localPaths.first();
	REQUIRE(restoredFile.startsWith(destinationDirectory.path() + "/library-media/"));
	REQUIRE(LibraryImporter::inspect(restoredFile).sha256 == imported.sha256);
	REQUIRE(QFile::exists(original));
	{
		LibraryStore reopened(destinationDirectory.filePath("library.sqlite"));
		REQUIRE(reopened.entry(key, collection).favorite);
		REQUIRE(reopened.entry(key).localPaths.contains(restoredFile));
	}
	REQUIRE(!QDir(destinationDirectory.path()).entryList({"library.sqlite.before-restore-*.bak"}, QDir::Files).isEmpty());
}

TEST_CASE("Invalid Library backups and failed media restores preserve current catalog", "[library][backup]")
{
	QTemporaryDir directory, incomingDirectory;
	REQUIRE(directory.isValid());
	REQUIRE(incomingDirectory.isValid());
	const QScopedPointer<Profile> profile(makeLibraryProfile(directory.path()));
	auto *store = profile->library();
	const qint64 collection = store->createCollection("Keep my collection");
	REQUIRE(collection > 0);
	profile->getSettings()->setValue("keep", "my setting");
	profile->getSettings()->sync();
	profile->addMd5("0123456789abcdef0123456789abcdef", "tests/resources/image_1x1.png");
	const QString archive = incomingDirectory.filePath("restore.zip");
	QHash<QString, QString> files;
	SECTION("Invalid Library is rejected before other files change")
	{
		const QString corrupt = incomingDirectory.filePath("library.sqlite");
		const QString settings = incomingDirectory.filePath("settings.ini");
		REQUIRE(safeWriteFile(corrupt, "not a database"));
		REQUIRE(safeWriteFile(settings, "keep=overwrite\n"));
		files.insert(corrupt, "library.sqlite");
		files.insert(settings, "settings.ini");
		REQUIRE(createZip(archive, files));
		REQUIRE(!loadBackup(profile.data(), archive));
		REQUIRE(profile->getSettings()->value("keep").toString() == "my setting");
	}
	SECTION("Failed file restore retains the old target and reopens MD5")
	{
		const QString source = incomingDirectory.filePath("settings.ini");
		REQUIRE(safeWriteFile(source, "new setting\n"));
		files.insert(source, "filenamehistory.txt");
		const QString destination = directory.filePath("filenamehistory.txt");
		REQUIRE(QDir().mkpath(destination));
		REQUIRE(safeWriteFile(QDir(destination).filePath("keep.txt"), "old data"));
		REQUIRE(createZip(archive, files));
		REQUIRE(!loadBackup(profile.data(), archive));
		REQUIRE(QFile::exists(QDir(destination).filePath("keep.txt")));
	}
	SECTION("Failed managed media restore retains the prior catalog")
	{
		LibraryStore incoming(incomingDirectory.filePath("library.sqlite"));
		REQUIRE(incoming.createCollection("Replace my collection") > 0);
		const QString snapshot = incomingDirectory.filePath("snapshot.sqlite");
		REQUIRE(incoming.backupTo(snapshot));
		const QString media = incomingDirectory.filePath("media.png");
		REQUIRE(safeWriteFile(media, "incoming media"));
		files.insert(snapshot, "library.sqlite");
		files.insert(media, "library-media/collision.png");
		const QString collision = directory.filePath("library-media/collision.png");
		REQUIRE(QDir().mkpath(collision));
		REQUIRE(safeWriteFile(QDir(collision).filePath("keep.txt"), "old data"));
		REQUIRE(createZip(archive, files));
		REQUIRE(!loadBackup(profile.data(), archive));
		REQUIRE(QFile::exists(QDir(collision).filePath("keep.txt")));
	}
	REQUIRE(profile->library() == store);
	REQUIRE(store->collections().size() == 1);
	REQUIRE(store->collections().first().name == "Keep my collection");
	REQUIRE(profile->md5Exists("0123456789abcdef0123456789abcdef").contains("tests/resources/image_1x1.png"));
}

TEST_CASE("Library restore rolls back when insertion fails", "[library][backup]")
{
	QTemporaryDir directory;
	REQUIRE(directory.isValid());
	const QString current = directory.filePath("current.sqlite");
	const QString incomingPath = directory.filePath("incoming.sqlite");
	LibraryStore store(current);
	REQUIRE(store.createCollection("Keep me") > 0);
	{
		LibraryStore incoming(incomingPath);
		REQUIRE(incoming.createCollection("Replacement") > 0);
	}
	{
		auto database = QSqlDatabase::addDatabase("QSQLITE", "backup-failure-test");
		database.setDatabaseName(current);
		REQUIRE(database.open());
		QSqlQuery query(database);
		REQUIRE(query.exec("CREATE TRIGGER fail_restore BEFORE INSERT ON collections BEGIN SELECT RAISE(ABORT, 'restore test failure'); END"));
		query.finish();
		database.close();
	}
	QSqlDatabase::removeDatabase("backup-failure-test");
	REQUIRE(!store.restoreFrom(incomingPath));
	REQUIRE(store.collections().size() == 1);
	REQUIRE(store.collections().first().name == "Keep me");
	REQUIRE(store.createCollection("Trigger still applies") == 0);
}


TEST_CASE("Library restores a damaged catalog while preserving its exact bytes", "[library][backup]")
{
	QTemporaryDir directory;
	const QString damaged = directory.filePath("damaged.sqlite"), good = directory.filePath("good.sqlite");
	REQUIRE(safeWriteFile(damaged, "precious damaged catalog"));
	{
		LibraryStore backup(good); REQUIRE(backup.isReady()); REQUIRE(backup.createCollection("Recovered") > 0);
	}
	LibraryStore current(damaged); REQUIRE_FALSE(current.isReady());
	REQUIRE(current.restoreFrom(good)); REQUIRE(current.isReady()); REQUIRE(current.collections().first().name == "Recovered");
	const auto backups = QDir(directory.path()).entryList({"damaged.sqlite.before-restore-*.bak"}, QDir::Files);
	REQUIRE(backups.size() == 1);
	QFile saved(directory.filePath(backups.first())); REQUIRE(saved.open(QIODevice::ReadOnly)); REQUIRE(saved.readAll() == "precious damaged catalog");
}
