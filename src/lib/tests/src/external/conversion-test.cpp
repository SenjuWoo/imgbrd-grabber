#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QScopeGuard>
#include "catch.h"
#include "external/ffmpeg.h"
#include "external/image-magick.h"


TEST_CASE("Conversions preserve originals and existing destinations", "[conversion]")
{
	QTemporaryDir directory;
	REQUIRE(directory.isValid());
	const QString input = directory.filePath("image.png");
	const QString output = directory.filePath("image.jpg");
	const QByteArray original("original input");
	const QByteArray previous("existing destination");
	QFile source(input);
	REQUIRE(source.open(QFile::WriteOnly));
	REQUIRE(source.write(original) == original.size());
	source.close();
	QFile target(output);
	REQUIRE(target.open(QFile::WriteOnly));
	REQUIRE(target.write(previous) == previous.size());
	target.close();

	// A missing optional backend must never clean up a user's existing file.
	const QByteArray previousPath = qgetenv("PATH");
	qputenv("PATH", directory.path().toUtf8());
	const auto restorePath = qScopeGuard([previousPath]() { qputenv("PATH", previousPath); });
	const int backend = GENERATE(0, 1, 2);
	auto convert = [backend](const QString &file, const QString &extension, bool overwrite) {
		if (backend == 0) {
			return FFmpeg::convert(file, extension, overwrite, true, 100);
		}
		if (backend == 1) {
			return FFmpeg::remux(file, extension, overwrite, true, 100);
		}
		return ImageMagick::convert(file, extension, overwrite, true, 100);
	};

	SECTION("Missing backend retains both files")
	{
		REQUIRE(convert(input, "jpg", true) == input);
	}
	SECTION("Disallowed overwrite retains both files")
	{
		REQUIRE(convert(input, "jpg", false) == input);
	}
	SECTION("Same-file conversion is a safe no-op")
	{
		REQUIRE(convert(input, "png", true) == input);
	}
	REQUIRE(source.open(QFile::ReadOnly));
	REQUIRE(source.readAll() == original);
	source.close();
	REQUIRE(target.open(QFile::ReadOnly));
	REQUIRE(target.readAll() == previous);
	target.close();
	REQUIRE(QDir(directory.path()).entryList(QDir::Dirs | QDir::NoDotAndDotDot).isEmpty());
}
