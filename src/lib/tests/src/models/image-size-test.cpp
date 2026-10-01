#include <QFile>
#include <QJsonObject>
#include <QTemporaryFile>
#include <QTemporaryDir>
#include "models/image-size.h"
#include "catch.h"


TEST_CASE("ImageSize")
{
	SECTION("TemporaryPath")
	{
		QFile file1("tests/resources/tmp/tmp1.txt");
		REQUIRE(file1.open(QFile::Truncate | QFile::WriteOnly | QFile::Text));
		file1.write("test");
		file1.close();

		QFile file2("tests/resources/tmp/tmp2.txt");
		REQUIRE(file2.open(QFile::Truncate | QFile::WriteOnly | QFile::Text));
		file2.write("test");
		file2.close();

		{
			ImageSize is;

			REQUIRE(is.setTemporaryPath(file1.fileName()));
			REQUIRE(!is.setTemporaryPath(file1.fileName()));
			REQUIRE(is.fileSize == 4);

			REQUIRE(is.setTemporaryPath(file2.fileName()));
			REQUIRE(!file1.exists());
		}

		REQUIRE(!file2.exists());
	}

	SECTION("SavePath")
	{
		QTemporaryFile file;
		REQUIRE(file.open());
		file.write("test");
		file.close();

		ImageSize is;
		REQUIRE(is.setSavePath(file.fileName()));
		REQUIRE(!is.setSavePath(file.fileName()));
		REQUIRE(is.fileSize == 4);
	}

	SECTION("SaveDefault")
	{
		QTemporaryDir directory;
		REQUIRE(directory.isValid());
		const QString dest = directory.filePath("image-size.jpg");

		ImageSize is;
		REQUIRE(is.save(dest) == QString());
		REQUIRE(!QFile::exists(dest));
	}

	SECTION("SaveMove")
	{
		QTemporaryDir directory;
		REQUIRE(directory.isValid());
		const QString dest = directory.filePath("image-size.jpg");

		QFile file(directory.filePath("source.tmp"));
		REQUIRE(file.open(QIODevice::WriteOnly));
		file.write("test");
		file.close();

		ImageSize is;
		REQUIRE(is.setTemporaryPath(file.fileName()));
		REQUIRE(is.save(dest) == file.fileName());

		REQUIRE(!file.exists());
		REQUIRE(QFile::exists(dest));
		REQUIRE(QFile::remove(dest));
	}

	SECTION("SaveCopy")
	{
		QTemporaryDir directory;
		REQUIRE(directory.isValid());
		const QString dest = directory.filePath("image-size.jpg");

		QFile file(directory.filePath("source.tmp"));
		REQUIRE(file.open(QIODevice::WriteOnly));
		file.write("test");
		file.close();

		ImageSize is;
		REQUIRE(is.setSavePath(file.fileName()));
		REQUIRE(is.save(dest) == file.fileName());

		REQUIRE(file.exists());
		REQUIRE(QFile::exists(dest));
		REQUIRE(QFile::remove(dest));
	}

	SECTION("Failed save keeps the source and reports failure")
	{
		QTemporaryDir directory;
		REQUIRE(directory.isValid());
		const QString source = directory.filePath("source.png");
		REQUIRE(QFile::copy("tests/resources/image_1x1.png", source));
		const QString target = directory.filePath(QString(300, 'x') + ".png");
		ImageSize image;
		SECTION("Temporary file")
		{
			image.setTemporaryPath(source);
		}
		SECTION("Previously saved file")
		{
			image.setSavePath(source);
		}
		REQUIRE(image.save(target).isEmpty());
		REQUIRE(image.savePath() == source);
		REQUIRE(QFile::exists(source));
		REQUIRE(!QFile::exists(target));
	}

	SECTION("Unavailable files have no MD5 and a changed path invalidates the hash")
	{
		ImageSize image;
		image.setSavePath("non_existing_file.png");
		REQUIRE(image.md5().isEmpty());
		image.setSavePath("tests/resources/image_1x1.png");
		REQUIRE(image.md5() == "956ddde86fb5ce85218b21e2f49e5c50");
		QTemporaryFile emptyFile;
		REQUIRE(emptyFile.open());
		emptyFile.close();
		image.setSavePath(emptyFile.fileName());
		REQUIRE(image.md5() == "d41d8cd98f00b204e9800998ecf8427e");
	}

	SECTION("Pixmap")
	{
		QPixmap pix("tests/resources/image_1x1.png");

		ImageSize is;
		is.setPixmap(pix);

		REQUIRE(is.pixmap().toImage() == pix.toImage());
	}

	SECTION("PixmapRect")
	{
		QPixmap pix("tests/resources/image_200x200.png");

		ImageSize is;
		is.rect = QRect(0, 0, 20, 40);
		is.setPixmap(pix);

		REQUIRE(is.pixmap().size() == QSize(20, 40));
	}

	SECTION("MD5 calculation")
	{
		ImageSize is;
		REQUIRE(is.md5() == "");

		is.setSavePath("tests/resources/image_1x1.png");
		REQUIRE(is.md5() == "956ddde86fb5ce85218b21e2f49e5c50");
	}

	SECTION("Serialization")
	{
		ImageSize original;
		original.fileSize = 123456;
		original.size = QSize(800, 600);
		original.rect = QRect(10, 20, 30, 40);

		QJsonObject json;
		original.write(json);

		ImageSize dest;
		dest.read(json);

		REQUIRE(dest.fileSize == original.fileSize);
		REQUIRE(dest.size == original.size);
		REQUIRE(dest.rect == original.rect);
	}
}
