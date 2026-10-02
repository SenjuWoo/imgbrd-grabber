#include <QSignalSpy>
#include <QTemporaryFile>
#include <QUrl>
#include "custom-network-access-manager.h"
#include "updater/program-updater.h"
#include "catch.h"


TEST_CASE("Updater")
{
	ProgramUpdater updater;

	SECTION("CompareEqual")
	{
		REQUIRE(updater.compareVersions("1.0.0", "1.0.0") == 0);
		REQUIRE(updater.compareVersions("1.4.0", "1.4.0") == 0);
		REQUIRE(updater.compareVersions("1.4.7", "1.4.7") == 0);
	}

	SECTION("CompareEqualAlphas")
	{
		REQUIRE(updater.compareVersions("1.0.0a2", "1.0.0a2") == 0);
		REQUIRE(updater.compareVersions("1.4.0a2", "1.4.0a2") == 0);
		REQUIRE(updater.compareVersions("1.4.7a2", "1.4.7a2") == 0);
	}


	SECTION("CompareMinor")
	{
		REQUIRE(updater.compareVersions("1.0.1", "1.0.0") == 1);
		REQUIRE(updater.compareVersions("1.0.0", "1.0.1") == -1);
	}

	SECTION("CompareNormal")
	{
		REQUIRE(updater.compareVersions("1.1.0", "1.0.0") == 1);
		REQUIRE(updater.compareVersions("1.0.0", "1.1.0") == -1);
	}

	SECTION("CompareMajor")
	{
		REQUIRE(updater.compareVersions("2.0.0", "1.0.0") == 1);
		REQUIRE(updater.compareVersions("1.0.0", "2.0.0") == -1);
	}

	SECTION("CompareTen")
	{
		REQUIRE(updater.compareVersions("2.0.0", "1.10.0") == 1);
		REQUIRE(updater.compareVersions("1.10.0", "2.0.0") == -1);
	}

	SECTION("CompareMissing")
	{
		REQUIRE(updater.compareVersions("1.0.1", "1.0") == 1);
		REQUIRE(updater.compareVersions("1.0", "1.0.1") == -1);
	}


	SECTION("CompareAlphas")
	{
		REQUIRE(updater.compareVersions("1.0.0a3", "1.0.0a2") == 1);
		REQUIRE(updater.compareVersions("1.0.0a2", "1.0.0a3") == -1);
	}

	SECTION("CompareAlphaToNew")
	{
		REQUIRE(updater.compareVersions("1.0.0", "1.0.0a3") == 1);
		REQUIRE(updater.compareVersions("1.0.0a3", "1.0.0") == -1);
	}

	SECTION("CompareAlphaToOld")
	{
		REQUIRE(updater.compareVersions("1.0.0a3", "0.1.0") == 1);
		REQUIRE(updater.compareVersions("0.1.0", "1.0.0a3") == -1);
	}

	SECTION("CompareAlphaToBeta")
	{
		REQUIRE(updater.compareVersions("1.0.0b1", "1.0.0a3") == 1);
		REQUIRE(updater.compareVersions("1.0.0a3", "1.0.0b1") == -1);
	}

	SECTION("Compare legacy fork revisions and Woo upgrades")
	{
		REQUIRE(updater.compareVersions("7.14.0-fable.3", "7.14.0-fable.2") == 1);
		REQUIRE(updater.compareVersions("7.14.0-fable.2", "7.14.0-fable.3") == -1);
		REQUIRE(updater.compareVersions("7.14.0-fable.10", "7.14.0-fable.9") == 1);
		REQUIRE(updater.compareVersions("7.14.0-fable.3", "7.14.0-fable.3") == 0);
		REQUIRE(updater.compareVersions("7.14.0", "7.14.0-fable.3") == -1);
		REQUIRE(updater.compareVersions("7.14.1-fable.1", "7.14.0-fable.10") == 1);
		REQUIRE(updater.compareVersions("7.15.0-fable.1", "7.14.0-fable.10") == 1);
		REQUIRE(updater.compareVersions("7.15.0", "7.14.0-fable.5") == 1);
		REQUIRE(updater.compareVersions("7.14.0-fable.5", "7.15.0") == -1);
	}

	SECTION("Failed or malformed release responses are not up-to-date verdicts")
	{
		for (const QString &body : {QString("500"), QString("404"), QString("not JSON"), QString("{}"),
									QString(R"({"tag_name":"v999.0.0","html_url":"https://example.com/installer"})")}) {
			QTemporaryFile response;
			REQUIRE(response.open());
			response.write(body.toUtf8());
			response.flush();
			CustomNetworkAccessManager::NextFiles.enqueue(body == "500" || body == "404" ? body : response.fileName());
			QSignalSpy failed(&updater, &ProgramUpdater::failed);
			QSignalSpy finished(&updater, &ProgramUpdater::finished);
			updater.checkForUpdates();
			REQUIRE(failed.wait());
			REQUIRE(finished.isEmpty());
			REQUIRE(updater.latestUrl().isEmpty());
		}
	}

	#ifndef NIGHTLY
		SECTION("Release tags are independent of their display title")
		{
			QTemporaryFile response;
			REQUIRE(response.open());
			response.write(R"({"tag_name":"v999.0.0-fable.10","name":"Fable update: a descriptive title","html_url":"https://github.com/SenjuWoo/imgbrd-grabber/releases/tag/v999.0.0-fable.10"})");
			response.flush();
			CustomNetworkAccessManager::NextFiles.enqueue(response.fileName());
			QSignalSpy spy(&updater, &ProgramUpdater::finished);
			updater.checkForUpdates();
			REQUIRE(spy.wait());
			const auto result = spy.takeFirst();
			REQUIRE(result.at(0).toString() == "999.0.0-fable.10");
			REQUIRE(result.at(1).toBool());
			REQUIRE(updater.latestUrl().host() == "github.com");
			REQUIRE(updater.latestUrl().path().startsWith("/SenjuWoo/imgbrd-grabber/"));
		}
	#endif
}
