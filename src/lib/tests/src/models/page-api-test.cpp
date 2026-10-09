#include <QJSEngine>
#include <QMutex>
#include <QScopedPointer>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryFile>
#include <QUrlQuery>
#include "custom-network-access-manager.h"
#include "models/api/javascript-api.h"
#include "models/page.h"
#include "models/page-api.h"
#include "models/profile.h"
#include "models/site.h"
#include "models/source.h"
#include "tags/tag.h"
#include "catch.h"
#include "source-helpers.h"


TEST_CASE("PageApi")
{
	setupSource("Danbooru (2.0)");
	setupSite("Danbooru (2.0)", "danbooru.donmai.us");

	setupSource("Gelbooru (0.2)");
	setupSite("Gelbooru (0.2)", "gelbooru.com");

	QString path = "tests/resources/sites/Danbooru (2.0)/danbooru.donmai.us/defaults.ini";
	QSettings settings(path, QSettings::IniFormat);
	settings.setValue("auth/pseudo", "user");
	settings.setValue("auth/apiKey", "test-api-key");
	settings.sync();

	const QScopedPointer<Profile> pProfile(makeProfile());
	auto *profile = pProfile.data();

	QList<Site*> sites { profile->getSites().value("danbooru.donmai.us") };
	REQUIRE(sites[0] != nullptr);

	SECTION("ParseUrlBasic")
	{
		Site *site = profile->getSites().value("gelbooru.com");
		REQUIRE(site != nullptr);

		QStringList tags = QStringList() << "test" << "tag";
		Page page(profile, site, sites, tags);
		PageApi pageApi(&page, profile, site, site->getApis().first(), tags);

		REQUIRE(pageApi.url().toString() == QString("https://gelbooru.com/index.php?page=dapi&s=post&q=index&limit=25&pid=0&tags=test tag"));
	}

	SECTION("ParseUrlLogin")
	{
		Site *site = sites.first();

		QStringList tags = QStringList() << "test" << "tag";
		Page page(profile, site, sites, tags);
		PageApi pageApi(&page, profile, site, site->getApis().first(), tags);

		REQUIRE(pageApi.url().toString() == QString("https://danbooru.donmai.us/posts.xml?limit=25&page=1&tags=test tag&login=user&api_key=test-api-key"));
	}

	SECTION("ParseUrlAltPage")
	{
		Site *site = sites.first();

		QStringList tags = QStringList() << "test" << "tag";
		Page prevPage(profile, site, sites, tags, 1000);
		Page page(profile, site, sites, tags, 1001);
		PageApi pageApi(&page, profile, site, site->getApis().first(), tags, 1001);
		pageApi.setLastPage(prevPage.pageInformation());

		REQUIRE(pageApi.url().toString() == QString("https://danbooru.donmai.us/posts.xml?limit=25&page=b0&tags=test tag&login=user&api_key=test-api-key"));
	}
}

TEST_CASE("PageApi pagination uses response evidence and the effective API limit")
{
	setupSource("Danbooru (2.0)");
	setupSite("Danbooru (2.0)", "danbooru.donmai.us");
	const QScopedPointer<Profile> profile(makeProfile());
	auto *site = profile->getSites().value("danbooru.donmai.us");
	REQUIRE(site != nullptr);
	site->setAutoLogin(false);
	Page parentPage(profile.data(), site, {site}, QStringList{"forest"}, 2, 400);
	REQUIRE(parentPage.imagesPerPage() == 200);
	REQUIRE(QUrlQuery(parentPage.url()).queryItemValue("limit") == "200");

	QJSEngine engine;
	QMutex mutex;
	QJSValue source = engine.evaluate(R"(({
		apis: {json: {maxLimit: 2, search: {
			url: function(query, opts) { return "/posts.json?page=" + query.page + "&limit=" + opts.limit; },
			parse: function(src) { return JSON.parse(src); }
		}}}
	}))");
	JavascriptApi api(&engine, source, &mutex, "json");
	PageApi page(&parentPage, profile.data(), site, &api, QStringList{"forest"}, 2, 400);
	REQUIRE(page.imagesPerPage() == 2);
	REQUIRE(QUrlQuery(page.url()).queryItemValue("limit") == "2");
	REQUIRE_FALSE(page.hasNext());

	QTemporaryFile response;
	REQUIRE(response.open());
	auto load = [&](const QByteArray &json) {
		REQUIRE(response.resize(0));
		REQUIRE(response.seek(0));
		REQUIRE(response.write(json) == json.size());
		REQUIRE(response.flush());
		CustomNetworkAccessManager::NextFiles.enqueue(response.fileName());
		QSignalSpy finished(&page, &PageApi::finishedLoading);
		page.load();
		REQUIRE(finished.wait());
		REQUIRE(finished.size() == 1);
		REQUIRE(finished.first().at(1).value<PageApi::LoadResult>() == PageApi::Ok);
	};
	const QByteArray image = R"({"id":7,"md5":"0123456789abcdef0123456789abcdef","file_url":"https://example.com/forest.png","tags":"forest"})";

	SECTION("Tag upper bounds and short unknown pages do not end a search")
	{
		load("{\"images\":[" + image + "],\"tags\":[{\"name\":\"forest\",\"count\":1}]}");
		REQUIRE(page.imagesCount() == 1);
		REQUIRE(page.pagesCount() == 1);
		REQUIRE_FALSE(page.isImageCountSure());
		REQUIRE(page.hasNext());
		load(R"({"images":[]})");
		REQUIRE_FALSE(page.hasNext());
	}
	SECTION("Advancing cursor wins over a stale exact total")
	{
		load("{\"images\":[" + image + "],\"pageCount\":2,\"urlNextPage\":\"/posts.json?after=next\"}");
		REQUIRE(page.pagesCount(false) == 2);
		REQUIRE(page.hasNext());
		PageInformation previous = page.pageInformation();
		PageApi next(&parentPage, profile.data(), site, &api, QStringList{"forest"}, 3, 400, PostFilter(), false, nullptr, 0, previous);
		REQUIRE(QUrlQuery(next.url()).queryItemValue("after") == "next");
		// Reload must not retain the old cursor or mark an estimate as exact.
		load("{\"images\":[" + image + "],\"tags\":[{\"name\":\"forest\",\"count\":1}]}");
		REQUIRE(page.nextPage().isEmpty());
		REQUIRE_FALSE(page.isPageCountSure());
		REQUIRE_FALSE(page.isImageCountSure());
		REQUIRE(page.hasNext());
	}
	SECTION("Known final and empty pages stop, including a nonadvancing cursor")
	{
		load("{\"images\":[" + image + "],\"imageCount\":4}");
		REQUIRE(page.pagesCount(false) == 2);
		REQUIRE_FALSE(page.hasNext());
		load("{\"images\":[],\"urlNextPage\":\"" + page.url().toString().toUtf8() + "\"}");
		REQUIRE_FALSE(page.hasNext());
	}
	SECTION("A nonadvancing cursor stops even when the response contains pictures")
	{
		load("{\"images\":[" + image + "],\"urlNextPage\":\"" + page.url().toString().toUtf8() + "\"}");
		REQUIRE(page.pageImageCount() == 1);
		REQUIRE(page.pagesCount(false) == -1);
		REQUIRE_FALSE(page.hasNext());
	}
	SECTION("Post filtering never hides continuation evidence")
	{
		PageApi filtered(&parentPage, profile.data(), site, &api, QStringList{"forest"}, 2, 400, PostFilter(QStringList{"-forest"}));
		const QByteArray json = "{\"images\":[" + image + "]}";
		REQUIRE(response.write(json) == json.size());
		REQUIRE(response.flush());
		CustomNetworkAccessManager::NextFiles.enqueue(response.fileName());
		QSignalSpy finished(&filtered, &PageApi::finishedLoading);
		filtered.load();
		REQUIRE(finished.wait());
		REQUIRE(filtered.images().isEmpty());
		REQUIRE(filtered.pageImageCount() == 1);
		REQUIRE(filtered.filteredImageCount() == 1);
		REQUIRE(filtered.hasNext());
	}
	SECTION("Forced API page size still honors the requested smart result cap")
	{
		source.property("apis").property("json").setProperty("forcedLimit", 5);
		PageApi forced(&parentPage, profile.data(), site, &api, QStringList{"forest"}, 1, 1, PostFilter(), true);
		REQUIRE(forced.imagesPerPage() == 5);
		const QByteArray json = "{\"images\":[" + image + "," + image + "],\"imageCount\":10}";
		REQUIRE(response.write(json) == json.size());
		REQUIRE(response.flush());
		CustomNetworkAccessManager::NextFiles.enqueue(response.fileName());
		QSignalSpy finished(&forced, &PageApi::finishedLoading);
		forced.load();
		REQUIRE(finished.wait());
		REQUIRE(forced.images().size() == 1);
		REQUIRE(forced.pageImageCount() == 2);
		REQUIRE(forced.pagesCount(false) == 2);
		REQUIRE(forced.hasNext());
	}
}
