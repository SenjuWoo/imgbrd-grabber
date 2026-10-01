#include <QCryptographicHash>
#include <QColor>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include "models/library-importer.h"
#include "catch.h"


namespace
{
	void writeImportFixture(const QString &path, const QByteArray &bytes)
	{
		QFile file(path);
		REQUIRE(file.open(QFile::WriteOnly));
		REQUIRE(file.write(bytes) == bytes.size());
	}

	QByteArray readImportFixture(const QString &path)
	{
		QFile file(path);
		REQUIRE(file.open(QFile::ReadOnly));
		return file.readAll();
	}

	QImage importFixture()
	{
		QImage image(90, 80, QImage::Format_RGB32);
		for (int row = 0; row < image.height(); ++row) {
			for (int column = 0; column < image.width(); ++column) {
				image.setPixelColor(column, row, QColor(column * 255 / 89, row * 255 / 79, 120));
			}
		}
		return image;
	}
}


TEST_CASE("Library import reads local evidence and preserves original image bytes", "[library][import]")
{
	QTemporaryDir directory;
	REQUIRE(directory.isValid());
	const QString originalMd5 = "0123456789abcdef0123456789abcdef";
	const QString path = directory.filePath(originalMd5 + ".png");
	QImage image = importFixture();
	image.setText("Source", "https://www.pixiv.net/artworks/123456");
	image.setText("Keywords", "embedded_tag forest");
	REQUIRE(image.save(path, "PNG"));
	const QByteArray original = readImportFixture(path);
	writeImportFixture(path + ".json", R"({"title":"Imported illustration","tags":["blue_hair","forest"],"sources":["https://danbooru.donmai.us/posts/123#fragment","file:///private/image.png","javascript:bad"]})");
	writeImportFixture(directory.filePath(originalMd5 + ".xmp"), R"(<x:xmpmeta xmlns:x="adobe:ns:meta/" xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#" xmlns:dc="http://purl.org/dc/elements/1.1/"><rdf:RDF><rdf:Description><dc:subject><rdf:Bag><rdf:li>xmp_tag</rdf:li></rdf:Bag></dc:subject><dc:source>https://example.test/source/42</dc:source></rdf:Description></rdf:RDF></x:xmpmeta>)");
	writeImportFixture(directory.filePath(originalMd5 + ".txt"), "text_tag, sky clouds\n");
	#ifdef Q_OS_WIN
		writeImportFixture(path + ":Zone.Identifier", "[ZoneTransfer]\nZoneId=3\nHostUrl=https://cdn.example.test/original.png\nReferrerUrl=https://example.test/post/42\n");
	#endif
	const auto imported = LibraryImporter::inspect(path);
	REQUIRE(imported.error.isEmpty());
	REQUIRE(imported.sha256 == QString::fromLatin1(QCryptographicHash::hash(original, QCryptographicHash::Sha256).toHex()));
	REQUIRE(imported.md5 == QString::fromLatin1(QCryptographicHash::hash(original, QCryptographicHash::Md5).toHex()));
	REQUIRE(imported.sourceMd5 == originalMd5);
	REQUIRE(imported.title == "Imported illustration");
	REQUIRE(imported.size == image.size());
	REQUIRE(imported.sourceUrls.contains("https://www.pixiv.net/artworks/123456"));
	REQUIRE(imported.sourceUrls.contains("https://danbooru.donmai.us/posts/123"));
	REQUIRE(imported.sourceUrls.contains("https://example.test/source/42"));
	REQUIRE(imported.tags.contains("embedded_tag"));
	REQUIRE(imported.tags.contains("blue_hair"));
	REQUIRE(imported.tags.contains("xmp_tag"));
	REQUIRE(imported.tags.contains("text_tag"));
	REQUIRE(imported.tags.count("forest") == 1);
	REQUIRE(imported.metadata.value("evidence").toArray().size() >= 5);
	REQUIRE(!imported.copied);
	REQUIRE(readImportFixture(path) == original);
	QImage preview;
	REQUIRE(preview.loadFromData(imported.thumbnail, "PNG"));
	REQUIRE(preview.width() <= 512);
	REQUIRE(preview.height() <= 512);
	#ifdef Q_OS_WIN
		REQUIRE(imported.sourceUrls.contains("https://cdn.example.test/original.png"));
		REQUIRE(imported.sourceUrls.contains("https://example.test/post/42"));
	#endif
}


TEST_CASE("Library import tolerates malformed and oversized sidecars without inventing metadata", "[library][import]")
{
	QTemporaryDir directory;
	REQUIRE(directory.isValid());
	const QString path = directory.filePath(QString::fromUtf8("\xc3\xa9tude-\xe6\xa3\xae.png"));
	REQUIRE(importFixture().save(path, "PNG"));
	writeImportFixture(path + ".json", "{broken json");
	writeImportFixture(path + ".xmp", "<!DOCTYPE x [<!ENTITY malicious 'tag'>]><x>&malicious;</x>");
	writeImportFixture(path + ".txt", QByteArray(256 * 1024 + 1, 'a'));
	const auto imported = LibraryImporter::inspect(path);
	REQUIRE(imported.error.isEmpty());
	REQUIRE(imported.path == path);
	REQUIRE(imported.sourceMd5.isEmpty());
	REQUIRE(imported.tags.isEmpty());
	REQUIRE(imported.sourceUrls.isEmpty());
	int errors = 0;
	for (const auto &evidence : imported.metadata.value("evidence").toArray()) {
		if (evidence.toObject().contains("error")) {
			++errors;
		}
	}
	REQUIRE(errors >= 3);
	REQUIRE(!LibraryImporter::inspect(directory.filePath("missing.png")).error.isEmpty());
	writeImportFixture(directory.filePath("broken.png"), "not an image");
	REQUIRE(!LibraryImporter::inspect(directory.filePath("broken.png")).error.isEmpty());
}


TEST_CASE("Library import hashes duplicates and finds visual candidates locally", "[library][import]")
{
	QTemporaryDir directory;
	REQUIRE(directory.isValid());
	const QString path = directory.filePath("original.png");
	REQUIRE(importFixture().save(path, "PNG"));
	const QString duplicate = directory.filePath("copy.png");
	REQUIRE(QFile::copy(path, duplicate));
	const auto first = LibraryImporter::inspect(path);
	const auto second = LibraryImporter::inspect(duplicate);
	REQUIRE(first.error.isEmpty());
	REQUIRE(second.error.isEmpty());
	REQUIRE(first.sha256 == second.sha256);
	REQUIRE(first.md5 == second.md5);
	REQUIRE(first.visualHash == second.visualHash);
	REQUIRE(LibraryImporter::visualDistance(first.visualHash, second.visualHash) == 0);
	REQUIRE(LibraryImporter::visualHash(importFixture().scaled(180, 160)) == first.visualHash);
	REQUIRE(LibraryImporter::visualDistance("0000000000000000", "ffffffffffffffff") == 64);
	REQUIRE(LibraryImporter::visualDistance("invalid", first.visualHash) == -1);
	REQUIRE(LibraryImporter::visualDistance("10000000000000000", first.visualHash) == -1);
	REQUIRE(LibraryImporter::visualHash(QImage()).isEmpty());
	const QString rotated = directory.filePath("rotated.jpg");
	REQUIRE(importFixture().save(rotated, "JPEG"));
	QByteArray jpeg = readImportFixture(rotated);
	// Real EXIF Orientation=6 (clockwise 90 degrees), little-endian TIFF IFD.
	jpeg.insert(2, QByteArray::fromHex("ffe1002245786966000049492a0008000000010012010300010000000600000000000000"));
	writeImportFixture(rotated, jpeg);
	const auto oriented = LibraryImporter::inspect(rotated);
	REQUIRE(oriented.error.isEmpty());
	REQUIRE(oriented.size == QSize(80, 90));
	QImage orientedPreview;
	REQUIRE(orientedPreview.loadFromData(oriented.thumbnail));
	REQUIRE(orientedPreview.height() > orientedPreview.width());
}


TEST_CASE("Library import safely copies, deduplicates, enumerates and cancels", "[library][import]")
{
	QTemporaryDir directory;
	REQUIRE(directory.isValid());
	const QString path = directory.filePath("original.png");
	REQUIRE(importFixture().save(path, "PNG"));
	const QByteArray original = readImportFixture(path);
	REQUIRE(QDir(directory.path()).mkpath("nested"));
	const QString nested = directory.filePath("nested/second.png");
	REQUIRE(QFile::copy(path, nested));
	writeImportFixture(directory.filePath("ignored.txt"), "tags");
	const QStringList files = LibraryImporter::imageFiles({ directory.path(), nested });
	REQUIRE(files.size() == 2);
	REQUIRE(files.contains(QFileInfo(path).canonicalFilePath()));
	REQUIRE(files.contains(QFileInfo(nested).canonicalFilePath()));
	#ifdef Q_OS_UNIX
		REQUIRE(QFile::link(directory.path(), directory.filePath("nested/loop")));
		REQUIRE(LibraryImporter::imageFiles({ directory.path() }).size() == 2);
	#endif
	const QString managed = directory.filePath("managed");
	const auto imported = LibraryImporter::inspect(path, managed);
	REQUIRE(imported.error.isEmpty());
	REQUIRE(imported.copied);
	REQUIRE(QFileInfo(imported.path).suffix() == "png");
	REQUIRE(imported.path.startsWith(managed + "/"));
	REQUIRE(readImportFixture(imported.path) == original);
	const auto duplicate = LibraryImporter::inspect(nested, managed);
	REQUIRE(duplicate.error.isEmpty());
	REQUIRE(duplicate.path == imported.path);
	REQUIRE(QDir(managed).entryList(QDir::Files).size() == 1);
	writeImportFixture(imported.path, "unmatched existing data");
	const auto collision = LibraryImporter::inspect(path, managed);
	REQUIRE(!collision.error.isEmpty());
	REQUIRE(readImportFixture(imported.path) == "unmatched existing data");
	REQUIRE(readImportFixture(path) == original);
	auto cancel = std::make_shared<std::atomic_bool>(true);
	REQUIRE(LibraryImporter::imageFiles({ directory.path() }, cancel).isEmpty());
	const QString cancelledDirectory = directory.filePath("cancelled");
	REQUIRE(!LibraryImporter::inspect(path, cancelledDirectory, cancel).error.isEmpty());
	REQUIRE(!QFileInfo::exists(cancelledDirectory));
	REQUIRE(readImportFixture(path) == original);
}
