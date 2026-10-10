#include <QDir>
#include <QFile>
#include <QJsonObject>
#include <QPixmap>
#include <QScopedPointer>
#include <QSqlQuery>
#include <QTemporaryDir>
#include "models/image.h"
#include "models/library-store.h"
#include "models/profile.h"
#include "models/site.h"
#include "catch.h"
#include "source-helpers.h"


TEST_CASE("Library persists scoped preferences without duplicating pictures", "[library]")
{
	QTemporaryDir directory;
	REQUIRE(directory.isValid());
	const QScopedPointer<Profile> profile(makeLibraryProfile(directory.path()));
	Site *site = profile->getSites().value("danbooru.donmai.us");
	REQUIRE(site != nullptr);
	const QMap<QString, QString> details {
		{ "id", "123" }, { "md5", "0123456789abcdef0123456789abcdef" }, { "file_url", "https://cdn.test/image.png?token=old" },
		{ "page_url", "https://danbooru.donmai.us/posts/123" }, { "tags", "forest green" }, { "width", "640" }, { "height", "480" }
	};
	Image image(site, details, {{ "id", qulonglong(123) }}, {}, profile.data());
	QPixmap thumbnail(64, 48);
	thumbnail.fill(Qt::green);
	image.setPreviewImage(thumbnail);
	const QString path = directory.filePath("catalog.sqlite");
	QString key;
	qint64 wallpapers;
	qint64 studies;
	{
		LibraryStore store(path);
		REQUIRE(store.isReady());
		key = store.saveImage(image);
		REQUIRE(!key.isEmpty());
		wallpapers = store.createCollection("Wallpapers");
		studies = store.createCollection("Color studies");
		REQUIRE(wallpapers > 0);
		REQUIRE(studies > 0);
		REQUIRE(store.createCollection("wallpapers") == 0);
		REQUIRE(store.createCollection("   ") == 0);
		REQUIRE(store.addToCollection(key, wallpapers));
		REQUIRE(store.addToCollection(key, wallpapers));
		REQUIRE(store.addToCollection(key, studies));
		REQUIRE(store.setLiked(key, true));
		REQUIRE(store.setFavorite(key, true, wallpapers));
		REQUIRE(store.setNotes(key, "Crop to 16:9", wallpapers));
		REQUIRE(!store.entry(key).favorite);
		REQUIRE(!store.entry(key, studies).favorite);
		REQUIRE(!store.entry(key, wallpapers).liked);
		REQUIRE(store.entries().size() == 1);
		REQUIRE(store.entry(key).collectionCount == 2);
		REQUIRE(store.setCollectionCover(wallpapers, key));
		REQUIRE(!store.addToCollection(key, 99999)); // Real SQLite foreign-key failure.
		REQUIRE(store.entry(key).collectionCount == 2);
		QJsonObject json;
		image.write(json);
		Image restored(profile.data());
		REQUIRE(restored.read(json, profile->getSites()));
		REQUIRE(restored.pageUrl() == image.pageUrl());
		REQUIRE(LibraryStore::imageKey(restored) == key);
		REQUIRE(store.saveImage(restored) == key); // Missing fresh preview must retain cached thumbnail and preferences.
		REQUIRE(store.entry(key).liked);
		REQUIRE(!store.entry(key).thumbnail.isEmpty());
	}
	{
		QTemporaryDir relocated;
		REQUIRE(QFile::copy(path, relocated.filePath("library.sqlite")));
		LibraryStore moved(relocated.filePath("library.sqlite"));
		REQUIRE(moved.isReady());
		REQUIRE(moved.entry(key, wallpapers).favorite);
		REQUIRE(!moved.entry(key).thumbnail.isEmpty());
		LibraryStore reopened(path);
		REQUIRE(reopened.isReady());
		REQUIRE(reopened.entry(key).liked);
		REQUIRE(reopened.entry(key, wallpapers).favorite);
		REQUIRE(reopened.entry(key, wallpapers).notes == "Crop to 16:9");
		REQUIRE(!reopened.entry(key, studies).favorite);
		REQUIRE(reopened.renameCollection(studies, "Lighting studies"));
		REQUIRE(reopened.removeCollection(wallpapers));
		REQUIRE(reopened.contains(key));
		REQUIRE(reopened.entry(key).collectionCount == 1);
		REQUIRE(reopened.removeFromCollection(key, studies));
		REQUIRE(reopened.entry(key).collectionCount == 0);
		QFile original(directory.filePath("original.png"));
		REQUIRE(original.open(QIODevice::WriteOnly));
		original.write("user-owned original");
		original.close();
		profile->addMd5(image.md5(), original.fileName());
		REQUIRE(reopened.removeImage(key));
		REQUIRE(reopened.entries().isEmpty());
		REQUIRE(original.exists());
		REQUIRE(profile->md5Exists(image.md5()).contains(original.fileName()));
	}
}

TEST_CASE("Library identity separates sites and gallery attachments", "[library]")
{
	QTemporaryDir directory;
	const QScopedPointer<Profile> profile(makeLibraryProfile(directory.path()));
	Site *site = profile->getSites().value("danbooru.donmai.us");
	REQUIRE(site != nullptr);
	const QMap<QString, QString> details { { "id", "42" }, { "file_url", "https://cdn.test/a.png" } };
	const auto gallery = QSharedPointer<Image>::create(site, details, profile.data());
	Image a(site, details, profile.data());
	Image b(site, {{ "id", "42" }, { "file_url", "https://other-cdn.test/b.png" }}, profile.data());
	a.setParentGallery(gallery);
	b.setParentGallery(gallery);
	REQUIRE(LibraryStore::imageKey(a) != LibraryStore::imageKey(b));
	QJsonObject json;
	a.write(json);
	Image restored(profile.data());
	REQUIRE(restored.read(json, profile->getSites()));
	REQUIRE(LibraryStore::imageKey(a) == LibraryStore::imageKey(restored));
	Site other("other.test", profile->getSources().value("Danbooru (2.0)"), profile.data());
	Image elsewhere(&other, details, profile.data());
	REQUIRE(LibraryStore::imageKey(*gallery) != LibraryStore::imageKey(elsewhere));
	Image changedCdn(site, {{ "id", "42" }, { "file_url", "https://new-cdn.test/a.png?new=token" }}, profile.data());
	REQUIRE(LibraryStore::imageKey(*gallery) == LibraryStore::imageKey(changedCdn));
}

TEST_CASE("Library recognizes rated pictures by identity and by file across sources", "[library]")
{
	QTemporaryDir directory;
	const QScopedPointer<Profile> profile(makeLibraryProfile(directory.path()));
	Site *site = profile->getSites().value("danbooru.donmai.us");
	REQUIRE(site != nullptr);
	const QString md5 = "0123456789abcdef0123456789abcdef";
	Image image(site, {{ "id", "7" }, { "md5", md5 }, { "file_url", "https://cdn.test/a.png" }}, profile.data());
	Site other("other.test", profile->getSources().value("Danbooru (2.0)"), profile.data());
	Image mirror(&other, {{ "id", "900" }, { "md5", md5.toUpper() }, { "file_url", "https://mirror.test/a.png" }}, profile.data());
	Image unrelated(&other, {{ "id", "901" }, { "md5", "ffffffffffffffffffffffffffffffff" }, { "file_url", "https://mirror.test/b.png" }}, profile.data());
	LibraryStore store(directory.filePath("rated.sqlite"));
	REQUIRE(store.isReady());
	REQUIRE_FALSE(store.isRated(image));
	const QString key = store.saveImage(image);
	REQUIRE_FALSE(store.isRated(image)); // Saved alone is not a rating.
	REQUIRE(store.setLiked(key, true));
	REQUIRE(store.isRated(image));
	REQUIRE(store.isRated(mirror));
	REQUIRE_FALSE(store.isRated(unrelated));
	REQUIRE(store.setLiked(key, false));
	REQUIRE_FALSE(store.isRated(mirror));
	const qint64 collection = store.createCollection("Scoped");
	REQUIRE(store.addToCollection(key, collection));
	REQUIRE(store.setFavorite(key, true, collection));
	REQUIRE(store.isRated(image));
}

TEST_CASE("Library preserves corrupt and unrecognized databases", "[library]")
{
	QTemporaryDir directory;
	const QString corrupt = directory.filePath("corrupt.sqlite");
	QFile file(corrupt);
	REQUIRE(file.open(QIODevice::WriteOnly));
	file.write("precious damaged bytes");
	file.close();
	{
		LibraryStore store(corrupt);
		REQUIRE(!store.isReady());
		REQUIRE(!store.lastError().isEmpty());
		REQUIRE(store.createCollection("Do not overwrite") == 0);
	}
	REQUIRE(file.open(QIODevice::ReadOnly));
	REQUIRE(file.readAll() == "precious damaged bytes");
	file.close();
	for (const bool future : { false, true }) {
		const QString path = directory.filePath(future ? "future.sqlite" : "unknown.sqlite");
		{
			auto db = QSqlDatabase::addDatabase("QSQLITE", "library-schema-test");
			db.setDatabaseName(path);
			REQUIRE(db.open());
			QSqlQuery query(db);
			REQUIRE(query.exec("CREATE TABLE precious (value TEXT)"));
			REQUIRE(query.exec("INSERT INTO precious VALUES ('keep me')"));
			if (future) {
				REQUIRE(query.exec("PRAGMA user_version=99"));
			}
			query.finish();
			db.close();
		}
		QSqlDatabase::removeDatabase("library-schema-test");
		QFile bytes(path);
		REQUIRE(bytes.open(QIODevice::ReadOnly));
		const QByteArray before = bytes.readAll();
		bytes.close();
		{
			LibraryStore store(path);
			REQUIRE(!store.isReady());
			REQUIRE(store.createCollection("New") == 0);
		}
		REQUIRE(bytes.open(QIODevice::ReadOnly));
		REQUIRE(bytes.readAll() == before);
	}
}

TEST_CASE("Library ratings are exclusive and independent in every scope", "[library]")
{
	QTemporaryDir directory;
	const QScopedPointer<Profile> profile(makeLibraryProfile(directory.path()));
	Site *site = profile->getSites().value("danbooru.donmai.us");
	REQUIRE(site != nullptr);
	Image image(site, {{"id", "72"}, {"file_url", "https://test.invalid/exclusive.png"}}, profile.data());
	const QString path = directory.filePath("ratings.sqlite");
	QString key;
	qint64 collection;
	{
		LibraryStore store(path);
		REQUIRE(store.isReady());
		key = store.saveImage(image);
		REQUIRE(!key.isEmpty());
		collection = store.createCollection("References");
		REQUIRE(collection > 0);
		REQUIRE(store.addToCollection(key, collection));
		REQUIRE(store.setNotes(key, "Global note"));
		REQUIRE(store.setNotes(key, "Collection note", collection));
		bool observedOverlap = false;
		QObject::connect(&store, &LibraryStore::imageChanged, &store, [&](const QString &) {
			for (const qint64 scope : {qint64(0), collection}) {
				const auto state = store.entry(key, scope);
				observedOverlap = observedOverlap || (state.liked && state.favorite);
			}
		});
		for (const qint64 scope : {qint64(0), collection}) {
			REQUIRE(store.setLiked(key, true, scope));
			REQUIRE(store.entry(key, scope).liked);
			REQUIRE_FALSE(store.entry(key, scope).favorite);
			REQUIRE(store.setFavorite(key, false, scope));
			REQUIRE(store.entry(key, scope).liked); // Clearing an inactive choice preserves the active one.
			REQUIRE(store.setFavorite(key, true, scope));
			REQUIRE_FALSE(store.entry(key, scope).liked);
			REQUIRE(store.entry(key, scope).favorite);
			REQUIRE(store.setLiked(key, false, scope));
			REQUIRE(store.entry(key, scope).favorite);
			REQUIRE(store.setLiked(key, true, scope));
			REQUIRE(store.entry(key, scope).liked);
			REQUIRE_FALSE(store.entry(key, scope).favorite);
			REQUIRE(store.setLiked(key, false, scope));
			REQUIRE_FALSE(store.entry(key, scope).liked);
			REQUIRE_FALSE(store.entry(key, scope).favorite);
			REQUIRE(store.setFavorite(key, true, scope));
			REQUIRE(store.setFavorite(key, false, scope));
			REQUIRE_FALSE(store.entry(key, scope).liked);
			REQUIRE_FALSE(store.entry(key, scope).favorite);
			REQUIRE(store.setFavorite(key, true, scope));
		}
		REQUIRE_FALSE(observedOverlap);
		REQUIRE(store.entry(key).favorite);
		REQUIRE(store.entry(key, collection).favorite);
		REQUIRE(store.setLiked(key, true, collection));
		REQUIRE(store.entry(key).favorite); // Collection choices do not alter global preferences.
		REQUIRE_FALSE(store.entry(key).liked);
		REQUIRE_FALSE(store.entry(key, collection).favorite);
		REQUIRE_FALSE(store.setFavorite(key, true, collection + 1));
		REQUIRE(store.entry(key).collectionCount == 1);
		REQUIRE(store.entry(key).notes == "Global note");
		REQUIRE(store.entry(key, collection).notes == "Collection note");
	}
	LibraryStore reopened(path);
	REQUIRE(reopened.isReady());
	REQUIRE(reopened.entry(key).favorite);
	REQUIRE_FALSE(reopened.entry(key).liked);
	REQUIRE(reopened.entry(key, collection).liked);
	REQUIRE_FALSE(reopened.entry(key, collection).favorite);
	REQUIRE(QDir(directory.path()).entryList({"ratings.sqlite.before-exclusive-ratings-*.bak"}, QDir::Files).isEmpty());
}

TEST_CASE("Legacy overlapping ratings retain favorites and a recoverable backup", "[library][backup]")
{
	QTemporaryDir directory;
	const QScopedPointer<Profile> profile(makeLibraryProfile(directory.path()));
	Site *site = profile->getSites().value("danbooru.donmai.us");
	REQUIRE(site != nullptr);
	Image image(site, {{"id", "73"}, {"file_url", "https://test.invalid/legacy.png"}}, profile.data());
	const QString path = directory.filePath("legacy.sqlite");
	QString key;
	qint64 collection;
	{
		LibraryStore original(path);
		key = original.saveImage(image);
		REQUIRE(!key.isEmpty());
		collection = original.createCollection("Keep this collection");
		REQUIRE(original.addToCollection(key, collection));
		REQUIRE(original.setNotes(key, "Keep global note"));
		REQUIRE(original.setNotes(key, "Keep scoped note", collection));
	}
	{
		auto db = QSqlDatabase::addDatabase("QSQLITE", "legacy-rating-fixture");
		db.setDatabaseName(path);
		REQUIRE(db.open());
		QSqlQuery query(db);
		REQUIRE(query.exec("UPDATE images SET liked=1,favorite=1"));
		REQUIRE(query.exec("UPDATE members SET liked=1,favorite=1"));
		query.finish();
		db.close();
	}
	QSqlDatabase::removeDatabase("legacy-rating-fixture");
	const QString restoreCopy = directory.filePath("incoming.sqlite");
	REQUIRE(QFile::copy(path, restoreCopy));
	QFile incoming(restoreCopy);
	REQUIRE(incoming.open(QIODevice::ReadOnly));
	const QByteArray incomingBytes = incoming.readAll();
	incoming.close();
	{
		LibraryStore normalized(path);
		REQUIRE(normalized.isReady());
		REQUIRE_FALSE(normalized.entry(key).liked);
		REQUIRE(normalized.entry(key).favorite);
		REQUIRE_FALSE(normalized.entry(key, collection).liked);
		REQUIRE(normalized.entry(key, collection).favorite);
		REQUIRE(normalized.entry(key).notes == "Keep global note");
		REQUIRE(normalized.entry(key, collection).notes == "Keep scoped note");
		REQUIRE(normalized.entry(key).collectionCount == 1);
		REQUIRE(normalized.collections().first().id == collection);
	}
	const auto backups = QDir(directory.path()).entryList({"legacy.sqlite.before-exclusive-ratings-*.bak"}, QDir::Files);
	REQUIRE(backups.size() == 1);
	{
		auto db = QSqlDatabase::addDatabase("QSQLITE", "legacy-rating-backup");
		db.setDatabaseName(directory.filePath(backups.first()));
		REQUIRE(db.open());
		QSqlQuery query(db);
		for (const QString &table : {QString("images"), QString("members")}) {
			REQUIRE(query.exec("SELECT liked,favorite,notes FROM " + table));
			REQUIRE(query.next());
			REQUIRE(query.value(0).toBool());
			REQUIRE(query.value(1).toBool());
			REQUIRE_FALSE(query.value(2).toString().isEmpty());
		}
		query.finish();
		db.close();
	}
	QSqlDatabase::removeDatabase("legacy-rating-backup");
	{
		LibraryStore reopened(path);
		REQUIRE(reopened.isReady());
	}
	REQUIRE(QDir(directory.path()).entryList({"legacy.sqlite.before-exclusive-ratings-*.bak"}, QDir::Files).size() == 1);
	for (const bool damaged : {false, true}) {
		const QString target = directory.filePath(damaged ? "damaged.sqlite" : "restore.sqlite");
		if (damaged) {
			QFile corrupt(target);
			REQUIRE(corrupt.open(QIODevice::WriteOnly));
			REQUIRE(corrupt.write("Keep damaged bytes") > 0);
		}
		LibraryStore restored(target);
		REQUIRE(restored.isReady() != damaged);
		REQUIRE(restored.restoreFrom(restoreCopy));
		REQUIRE(restored.isReady());
		REQUIRE_FALSE(restored.entry(key).liked);
		REQUIRE(restored.entry(key).favorite);
		REQUIRE_FALSE(restored.entry(key, collection).liked);
		REQUIRE(restored.entry(key, collection).favorite);
		REQUIRE(restored.entry(key).notes == "Keep global note");
		REQUIRE(restored.entry(key, collection).notes == "Keep scoped note");
		REQUIRE(restored.collections().first().id == collection);
	}
	REQUIRE(incoming.open(QIODevice::ReadOnly));
	REQUIRE(incoming.readAll() == incomingBytes); // Restore validation must not modify the supplied backup.
}
