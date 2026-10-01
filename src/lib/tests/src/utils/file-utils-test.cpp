#include <QDir>
#include <QFile>
#include <QString>
#include <QTemporaryFile>
#include <QTemporaryDir>
#include "catch.h"
#include "raii-helpers.h"
#include "utils/file-utils.h"


TEST_CASE("File utils")
{
	SECTION("copyRecursively")
	{
		QString from = QDir::toNativeSeparators("tests/resources/recurse/");
		QString to = QDir::toNativeSeparators("tests/resources/tmp/recurse/");

		DirectoryDeleter deleter(to, false, true);

		SECTION("Basic usage")
		{
			REQUIRE(copyRecursively(from, to));
			REQUIRE(QFile::exists(to + "test.txt"));
			REQUIRE(QFile::exists(to + "test/test1.txt"));
			REQUIRE(QFile::exists(to + "test/test2.txt"));
		}

		SECTION("Already exists")
		{
			REQUIRE(copyRecursively(from, to));
			REQUIRE(!copyRecursively(from, to));
		}

		SECTION("Overwrite")
		{
			REQUIRE(copyRecursively(from, to));
			REQUIRE(copyRecursively(from, to, true));
		}
	}

	SECTION("safeCopyFile")
	{
		const QString from = "tests/resources/image_1x1.png";
		const QString file = "tests/resources/tmp/safeCopy.txt";
		FileDeleter deleter(file, true);
		FileDeleter deleterBak(file + ".bak", true);

		SECTION("Basic usage")
		{
			REQUIRE(!QFile::exists(file));
			REQUIRE(safeCopyFile(from, file));
			REQUIRE(QFile::exists(file));
		}

		SECTION("Overwrite without backup")
		{
			REQUIRE(safeCopyFile(from, file, false));
			REQUIRE(QFile::exists(file));

			REQUIRE(safeCopyFile(from, file, false));
			REQUIRE(QFile::exists(file));
			REQUIRE(!QFile::exists(file + ".bak"));
		}

		SECTION("Overwrite with backup")
		{
			REQUIRE(safeCopyFile(from, file, false));
			REQUIRE(QFile::exists(file));

			REQUIRE(safeCopyFile(from, file, true));
			REQUIRE(QFile::exists(file));
			REQUIRE(QFile::exists(file + ".bak"));
		}
	}

	SECTION("Atomic copy preserves destinations on failure and unrelated backups")
	{
		QTemporaryDir directory;
		REQUIRE(directory.isValid());
		const QString input = directory.filePath("source.bin");
		const QString output = directory.filePath("output.bin");
		const QString backup = output + ".bak";
		const QByteArray content(1024 * 1024 + 17, 'x');
		REQUIRE(safeWriteFile(input, content));
		REQUIRE(safeWriteFile(output, "old"));
		REQUIRE(safeWriteFile(backup, "keep backup"));
		QByteArray expected("old");
		SECTION("Successful overwrite")
		{
			REQUIRE(atomicCopyFile(input, output));
			expected = content;
		}
		SECTION("Missing input")
		{
			REQUIRE(!atomicCopyFile(directory.filePath("missing"), output));
		}
		#ifdef Q_OS_LINUX
			SECTION("Read failure after opening")
			{
				REQUIRE(!atomicCopyFile("/proc/self/mem", output));
			}
		#endif
		QFile target(output);
		REQUIRE(target.open(QFile::ReadOnly));
		REQUIRE(target.readAll() == expected);
		QFile preservedBackup(backup);
		REQUIRE(preservedBackup.open(QFile::ReadOnly));
		REQUIRE(preservedBackup.readAll() == "keep backup");
		REQUIRE(QFile::exists(input));
	}

	SECTION("safeWriteFile")
	{
		const QString file = "tests/resources/tmp/safe.txt";
		FileDeleter deleter(file, true);
		FileDeleter deleterBak(file + ".bak", true);

		SECTION("Basic usage")
		{
			REQUIRE(!QFile::exists(file));
			REQUIRE(safeWriteFile(file, "test"));
			REQUIRE(QFile::exists(file));
		}

		SECTION("Overwrite without backup")
		{
			REQUIRE(safeWriteFile(file, "test", false));
			REQUIRE(QFile::exists(file));

			REQUIRE(safeWriteFile(file, "test", false));
			REQUIRE(QFile::exists(file));
			REQUIRE(!QFile::exists(file + ".bak"));
		}

		SECTION("Overwrite with backup")
		{
			REQUIRE(safeWriteFile(file, "test", false));
			REQUIRE(QFile::exists(file));

			REQUIRE(safeWriteFile(file, "test", true));
			REQUIRE(QFile::exists(file));
			REQUIRE(QFile::exists(file + ".bak"));
		}
	}

	SECTION("ensureFileParent")
	{
		SECTION("Parent doesn't exist")
		{
			const QString dir = "tests/resources/tmp/parent/";
			const QString file = dir + "ensure-parent.txt";
			DirectoryDeleter deleter(dir, false, true);

			REQUIRE(!QDir().exists(dir));
			REQUIRE(ensureFileParent(file));
			REQUIRE(QDir().exists(dir));
		}

		SECTION("Parent already exists")
		{
			const QString dir = "tests/resources/tmp/";
			const QString file = dir + "ensure-parent.txt";
			FileDeleter deleter(file, true);

			REQUIRE(QDir().exists(dir));
			REQUIRE(ensureFileParent(file));
			REQUIRE(QDir().exists(dir));
		}
	}

	SECTION("writeFile")
	{
		const QString file = "tests/resources/tmp/write.txt";
		FileDeleter deleter(file, true);

		REQUIRE(!QFile::exists(file));
		REQUIRE(writeFile(file, "test"));
		REQUIRE(QFile::exists(file));
	}
}
