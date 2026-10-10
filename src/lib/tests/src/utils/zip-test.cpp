#include <QFile>
#include <QDir>
#include <QTemporaryDir>
#include <QScopeGuard>
#include <vendor/miniz.h>
#include "utils/file-utils.h"
#include "catch.h"
#include "raii-helpers.h"
#include "utils/zip.h"


TEST_CASE("Zip")
{
	FileDeleter removeFile("tests/resources/test.zip", true);
	DirectoryDeleter removeDir("tests/resources/unzip-dir");

	SECTION("Create zip and unzip")
	{
		const QString filePath = "tests/resources/test.zip";
		const QHash<QString, QString> files = {
			{ "tests/resources/image_1x1.png", "1x1.png" },
			{ "tests/resources/image_200x200.png", "200x200.png" },
		};

		// Create ZIP
		REQUIRE(createZip(filePath, files));
		REQUIRE(QFile::exists(filePath));

		// Unzip
		REQUIRE(unzipFile(filePath, "tests/resources/unzip-dir"));
		REQUIRE(QFile::exists("tests/resources/unzip-dir/1x1.png"));
		REQUIRE(QFile::exists("tests/resources/unzip-dir/200x200.png"));
	}
}


namespace
{
	void rawTestZip(const QString &path, const QStringList &names)
	{
		QList<QByteArray> placeholders;
		QByteArray bytes;
		{
			mz_zip_archive archive {};
			REQUIRE(mz_zip_writer_init_heap(&archive, 0, 0));
			auto close = qScopeGuard([&]() { mz_zip_writer_end(&archive); });
			const QByteArray payload("incoming payload");
			for (int i = 0; i < names.size(); ++i) {
				QByteArray placeholder(names[i].toUtf8().size(), 'x');
				placeholder[0] = char('a' + i);
				placeholders.append(placeholder);
				REQUIRE(mz_zip_writer_add_mem(&archive, placeholder.constData(), payload.constData(), size_t(payload.size()), 0));
			}
			void *buffer = nullptr;
			size_t size = 0;
			REQUIRE(mz_zip_writer_finalize_heap_archive(&archive, &buffer, &size));
			bytes = QByteArray(static_cast<const char*>(buffer), qsizetype(size));
			mz_free(buffer);
		}
		// Miniz refuses absolute names when writing; mutate equal-length ZIP headers
		// to exercise archives supplied by other writers without bypassing the reader.
		for (int i = 0; i < names.size(); ++i) {
			REQUIRE(bytes.count(placeholders[i]) == 2); bytes.replace(placeholders[i], names[i].toUtf8());
		}
		QFile file(path); REQUIRE(file.open(QIODevice::WriteOnly));
		REQUIRE(file.write(bytes) == bytes.size());
	}

}

TEST_CASE("ZIP preflight rejects path escapes before extracting any files", "[zip][backup]")
{
	QTemporaryDir directory;
	REQUIRE(directory.isValid());
	const QString zip = directory.filePath("unsafe.zip"), target = directory.filePath("target");
	for (const QString &name : {QString("../escape.txt"), QString("/absolute.txt"), QString("C:/escape.txt"), QString("folder/../../escape.txt"), QString("..\\escape.txt"), QString("file.txt:stream"), QString("folder/.. /escape.txt"), QString("NUL")}) {
		rawTestZip(zip, {"good.txt", name});
		REQUIRE_FALSE(unzipFile(zip, target));
		REQUIRE_FALSE(QFile::exists(QDir(target).filePath("good.txt")));
		REQUIRE_FALSE(QFile::exists(directory.filePath("escape.txt")));
	}
}

TEST_CASE("ZIP failures preserve existing files and archives", "[zip][backup]")
{
	QTemporaryDir directory;
	REQUIRE(directory.isValid());
	const QString zip = directory.filePath("archive.zip"), target = directory.filePath("target");
	REQUIRE(QDir().mkpath(target));
	SECTION("Creation failure preserves the existing ZIP")
	{
		REQUIRE(safeWriteFile(zip, "prior archive"));
		REQUIRE_FALSE(createZip(zip, {{directory.filePath("missing"), "missing.txt"}}));
		QFile file(zip); REQUIRE(file.open(QIODevice::ReadOnly)); REQUIRE(file.readAll() == "prior archive");
		file.close();
		REQUIRE(QFile::remove(zip)); // Archive and staging handles have been closed.
	}
	SECTION("Failed extraction keeps a blocking parent file")
	{
		REQUIRE(safeWriteFile(QDir(target).filePath("folder"), "keep parent"));
		rawTestZip(zip, {"folder/image.txt"});
		REQUIRE_FALSE(unzipFile(zip, target));
		QFile file(QDir(target).filePath("folder")); REQUIRE(file.open(QIODevice::ReadOnly)); REQUIRE(file.readAll() == "keep parent");
	}
	SECTION("CRC failure keeps the prior extracted destination")
	{
		rawTestZip(zip, {"picture.txt"});
		QFile archive(zip); REQUIRE(archive.open(QIODevice::ReadWrite));
		QByteArray bytes = archive.readAll(); const int offset = int(bytes.indexOf("incoming payload")); REQUIRE(offset >= 0); bytes[offset] = 'X';
		REQUIRE(archive.seek(0)); REQUIRE(archive.write(bytes) == bytes.size()); archive.close();
		const QString original = QDir(target).filePath("picture.txt"); REQUIRE(safeWriteFile(original, "old picture"));
		REQUIRE_FALSE(unzipFile(zip, target));
		QFile file(original); REQUIRE(file.open(QIODevice::ReadOnly)); REQUIRE(file.readAll() == "old picture");
	}
}

TEST_CASE("ZIP backups support Unicode paths and nested files", "[zip][backup]")
{
	QTemporaryDir directory;
	REQUIRE(directory.isValid());
	const QString source = directory.filePath(QString::fromUtf8("\u753b-source.txt"));
	const QString archive = directory.filePath(QString::fromUtf8("\u753b-backup.zip"));
	const QString target = directory.filePath(QString::fromUtf8("\u753b-restored"));
	const QString name = QString::fromUtf8("folder/\u753b.txt");
	REQUIRE(safeWriteFile(source, "preserved bytes"));
	REQUIRE(createZip(archive, {{source, name}}));
	REQUIRE(unzipFile(archive, target));
	QFile file(QDir(target).filePath(name)); REQUIRE(file.open(QIODevice::ReadOnly)); REQUIRE(file.readAll() == "preserved bytes");
}
