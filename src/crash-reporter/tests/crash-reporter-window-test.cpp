#include <QDir>
#include <QFile>
#include <QLineEdit>
#include "catch.h"
#include "crash-reporter-window.h"
#include "raii-helpers.h"


TEST_CASE("CrashReporterWindow")
{
	REQUIRE(QDir().mkpath("tests/resources"));
	FileDeleter lastDump("tests/resources/lastdump");

	SECTION("Renders default values")
	{
		const QScopedPointer<CrashReporterWindow> window(new CrashReporterWindow(nullptr));

		const auto *lineLog = window->findChild<QLineEdit*>("lineLog");
		const auto *lineSettings = window->findChild<QLineEdit*>("lineSettings");
		const auto *lineDump = window->findChild<QLineEdit*>("lineDump");

		REQUIRE(lineLog->text().endsWith("main.log"));
		REQUIRE(lineSettings->text().endsWith("settings.ini"));
		REQUIRE(lineDump->text().isEmpty());
	}

	SECTION("Reads the 'lastdump' file if it exists")
	{
		QFile f("tests/resources/lastdump");
		REQUIRE(f.open(QFile::WriteOnly | QFile::Truncate));
		REQUIRE(f.write("test.dmp") == 8);
		f.close();

		const QScopedPointer<CrashReporterWindow> window(new CrashReporterWindow(nullptr));

		const auto *lineDump = window->findChild<QLineEdit*>("lineDump");
		REQUIRE(lineDump->text().endsWith("test.dmp"));
	}
}
