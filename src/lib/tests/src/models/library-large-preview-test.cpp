#include <QCryptographicHash>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QScopedPointer>
#include <QTemporaryDir>
#include "models/library-importer.h"
#include "models/library-store.h"
#include "models/profile.h"
#include "catch.h"
#include "source-helpers.h"

TEST_CASE("Oversized images retain metadata and independent Library preferences", "[library][import][large-preview]")
{
	QTemporaryDir directory;
	REQUIRE(directory.isValid());
	const QString md5 = "0123456789abcdef0123456789abcdef";
	const QString path = directory.filePath(md5 + ".png");
	{
		QImage pixels(8000, 6000, QImage::Format_Grayscale8);
		REQUIRE_FALSE(pixels.isNull());
		pixels.fill(Qt::white);
		REQUIRE(pixels.save(path, "PNG"));
	}
	QFile sidecar(path + ".json");
	REQUIRE(sidecar.open(QIODevice::WriteOnly));
	sidecar.write(R"({"tags":["large_picture"],"source":"https://example.test/post/42"})");
	sidecar.close();
	const auto imported = LibraryImporter::inspect(path);
	REQUIRE(imported.error.isEmpty());
	REQUIRE(imported.size == QSize(8000, 6000));
	REQUIRE(imported.thumbnail.isEmpty());
	REQUIRE(imported.visualHash.isEmpty());
	REQUIRE_FALSE(imported.metadata.value("preview_error").toString().isEmpty());
	REQUIRE(imported.sourceMd5 == md5);
	REQUIRE(imported.tags.contains("large_picture"));
	REQUIRE(imported.sourceUrls.contains("https://example.test/post/42"));
	const QScopedPointer<Profile> profile(makeLibraryProfile(directory.path()));
	auto *store = profile->library();
	const QString key = store->saveLocalImage(imported);
	REQUIRE_FALSE(key.isEmpty());
	const auto collection = store->createCollection("Large originals");
	REQUIRE(store->addToCollection(key, collection));
	REQUIRE(store->setLiked(key, true));
	REQUIRE(store->setFavorite(key, true, collection));
	REQUIRE(store->setNotes(key, "Keep original", collection));
	REQUIRE(store->entry(key).liked);
	REQUIRE_FALSE(store->entry(key).favorite);
	REQUIRE(store->entry(key, collection).favorite);
	REQUIRE(store->entry(key, collection).notes == "Keep original");
	REQUIRE(store->entry(key).metadataErrors().isEmpty());
	REQUIRE(store->entry(key).tags().contains("large_picture"));
	REQUIRE(store->entry(key).localPaths.contains(path));
	REQUIRE(store->saveLocalImage(imported) == key);
	REQUIRE(store->entries().size() == 1);
	REQUIRE(QFile::exists(path));
	// Optional read-only coverage of real large downloads, never part of the CI fixture.
	for (const auto &original : qEnvironmentVariable("GRABBER_TEST_LARGE_FILES").split('|', Qt::SkipEmptyParts)) {
		QFile file(original);
		REQUIRE(file.open(QIODevice::ReadOnly));
		const auto before = QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256);
		file.close();
		const auto actual = LibraryImporter::inspect(original);
		REQUIRE(actual.error.isEmpty());
		REQUIRE(actual.sha256 == QString::fromLatin1(before.toHex()));
		REQUIRE(actual.thumbnail.isEmpty());
		REQUIRE_FALSE(store->saveLocalImage(actual).isEmpty());
		REQUIRE(file.open(QIODevice::ReadOnly));
		REQUIRE(QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256) == before);
	}
}
