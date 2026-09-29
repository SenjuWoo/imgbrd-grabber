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
