#include <QDir>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QScopedPointer>
#include <QSqlQuery>
#include <QTemporaryDir>
#include "models/image.h"
#include "models/library-importer.h"
#include "models/library-store.h"
#include "models/profile.h"
#include "catch.h"
#include "source-helpers.h"

TEST_CASE("Local Library imports persist duplicates and independent preferences", "[library][import]")
{
	QTemporaryDir directory;
	QImage picture(200, 120, QImage::Format_RGB32);
	picture.fill(Qt::red);
	const QString original = directory.filePath("original.png");
	const QString duplicate = directory.filePath("renamed.png");
	REQUIRE(picture.save(original));
	REQUIRE(QFile::copy(original, duplicate));
	const auto data = LibraryImporter::inspect(original);
	REQUIRE(data.error.isEmpty());
	const QString path = directory.filePath("library.sqlite");
	QString key;
	qint64 collection;
	{
		LibraryStore store(path);
		REQUIRE(store.isReady());
		collection = store.createCollection("References");
		key = store.saveLocalImage(data, collection);
		INFO(store.lastError().toStdString());
		REQUIRE(!key.isEmpty());
		REQUIRE(store.setLiked(key, true));
		REQUIRE(store.setFavorite(key, true, collection));
		REQUIRE(store.setNotes(key, "Local note", collection));
		QFile tags(directory.filePath("renamed.txt"));
		REQUIRE(tags.open(QIODevice::WriteOnly));
		REQUIRE(tags.write("new_tag landscape") > 0);
		tags.close();
		REQUIRE(store.saveLocalImage(LibraryImporter::inspect(duplicate), collection) == key);
		REQUIRE(store.entry(key).image.value("tags").toString().contains("new_tag"));
		REQUIRE(store.entries().size() == 1);
		REQUIRE(store.entry(key).localPaths.size() == 2);
		REQUIRE(store.entry(key, collection).favorite);
		REQUIRE_FALSE(store.entry(key).favorite);
		REQUIRE_FALSE(store.entry(key, collection).liked);
		REQUIRE(store.entry(key).image.value("website").toString().isEmpty());
		picture.fill(Qt::blue);
		const QString different = directory.filePath("different.png");
		REQUIRE(picture.save(different));
		REQUIRE(store.saveLocalImage(LibraryImporter::inspect(different), 0, key).isEmpty());
		REQUIRE(store.entry(key).localPaths.size() == 2);
		REQUIRE(store.saveLocalImage(data, 999999).isEmpty());
		REQUIRE(store.entry(key).collectionCount == 1);
	}
	{
		LibraryStore reopened(path);
		REQUIRE(reopened.isReady());
		REQUIRE(reopened.entry(key).liked);
		REQUIRE(reopened.entry(key, collection).notes == "Local note");
		REQUIRE(reopened.removeImage(key));
		REQUIRE(QFile::exists(original));
		REQUIRE(QFile::exists(duplicate));
	}
}

TEST_CASE("Source linking merges catalog state and keeps one stable identity", "[library][import]")
{
	QTemporaryDir directory;
	const QScopedPointer<Profile> profile(makeLibraryProfile(directory.path()));
	LibraryStore *store = profile->library();
	QImage picture(200, 120, QImage::Format_RGB32);
	picture.fill(Qt::green);
	const QString file = directory.filePath("local.png");
	REQUIRE(picture.save(file));
	const auto local = LibraryImporter::inspect(file);
	const QString key = store->saveLocalImage(local);
	Site *site = profile->getSites().value("danbooru.donmai.us");
	Image source(site, {{"id", "432"}, {"md5", "11111111111111111111111111111111"}, {"file_url", "https://test.invalid/picture.png"}}, profile.data());
	source.setPreviewImage(QPixmap::fromImage(picture));
	const QString online = store->saveImage(source);
	REQUIRE(online != key);
	const qint64 a = store->createCollection("A");
	const qint64 b = store->createCollection("B");
	REQUIRE(store->addToCollection(key, a));
	REQUIRE(store->addToCollection(online, a));
	REQUIRE(store->addToCollection(online, b));
	REQUIRE(store->setLiked(key, true));
	REQUIRE(store->setFavorite(online, true));
	REQUIRE(store->setNotes(key, "First"));
	REQUIRE(store->setNotes(online, "Second"));
	REQUIRE(store->setLiked(online, true, a));
	REQUIRE(store->setLiked(online, true, b));
	REQUIRE(store->setFavorite(key, true, a));
	REQUIRE(store->setNotes(key, "Local", a));
	REQUIRE(store->setNotes(online, "Remote", a));
	REQUIRE(store->setCollectionCover(b, online));
	REQUIRE(store->linkSource(key, source, "User confirmed visual candidate"));
	REQUIRE(store->entries().size() == 1);
	REQUIRE(store->keyForImage(source) == key);
	REQUIRE_FALSE(store->contains(online));
	REQUIRE_FALSE(store->entry(key).liked);
	REQUIRE(store->entry(key).favorite);
	REQUIRE(store->entry(key).notes.contains("First"));
	REQUIRE(store->entry(key).notes.contains("Second"));
	REQUIRE_FALSE(store->entry(key, a).liked);
	REQUIRE(store->entry(key, a).favorite);
	REQUIRE(store->entry(key, a).notes.contains("Local"));
	REQUIRE(store->entry(key, a).notes.contains("Remote"));
	REQUIRE(store->entry(key).collectionCount == 2);
	REQUIRE(store->entry(key, b).liked);
	REQUIRE_FALSE(store->entry(key, b).favorite);
	REQUIRE(store->entry(key).localPaths.contains(file));
	REQUIRE(store->entry(key).image.value("local_import").toObject().value("sha256").toString() == local.sha256);
	REQUIRE(store->saveImage(source) == key);
	REQUIRE(store->entry(key).image.contains("local_import"));
	REQUIRE(store->saveLocalImage(local) == key);
	REQUIRE(store->collections().last().cover.size() > 0);
}

TEST_CASE("Schema one upgrade backs up and preserves the existing Library", "[library][import]")
{
	QTemporaryDir directory;
	const QString path = directory.filePath("library.sqlite");
	QString key;
	{
		LibraryStore original(path);
		QImage picture(20, 20, QImage::Format_RGB32);
		picture.fill(Qt::red);
		REQUIRE(picture.save(directory.filePath("original.png")));
		key = original.saveLocalImage(LibraryImporter::inspect(directory.filePath("original.png")));
		REQUIRE(original.setFavorite(key, true));
	}
	{
		auto db = QSqlDatabase::addDatabase("QSQLITE", "import-migration-fixture");
		db.setDatabaseName(path);
		REQUIRE(db.open());
		QSqlQuery query(db);
		REQUIRE(query.exec("DROP TABLE local_files"));
		REQUIRE(query.exec("DROP TABLE source_links"));
		REQUIRE(query.exec("PRAGMA user_version=1"));
		query.finish(); db.close();
	}
	QSqlDatabase::removeDatabase("import-migration-fixture");
	{
		LibraryStore upgraded(path);
		REQUIRE(upgraded.isReady());
		REQUIRE(upgraded.entry(key).favorite);
		REQUIRE(upgraded.entry(key).thumbnail.size() > 0);
		REQUIRE(upgraded.saveLocalImage(LibraryImporter::inspect(directory.filePath("original.png"))) == key);
	}
	const auto backups = QDir(directory.path()).entryList({"library.sqlite.before-import-*.bak"}, QDir::Files);
	REQUIRE(backups.size() == 1);
	{
		auto db = QSqlDatabase::addDatabase("QSQLITE", "import-backup-fixture");
		db.setDatabaseName(directory.filePath(backups.first()));
		REQUIRE(db.open());
		QSqlQuery query(db);
		REQUIRE(query.exec("PRAGMA user_version"));
		REQUIRE(query.next());
		REQUIRE(query.value(0).toInt() == 1);
		query.finish();
		query.prepare("SELECT favorite FROM images WHERE key=?");
		query.addBindValue(key);
		REQUIRE(query.exec());
		REQUIRE(query.next());
		REQUIRE(query.value(0).toBool());
		query.finish(); db.close();
	}
	QSqlDatabase::removeDatabase("import-backup-fixture");
}
