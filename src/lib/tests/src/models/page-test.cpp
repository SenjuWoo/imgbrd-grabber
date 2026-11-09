#include <QScopedPointer>
#include <QSignalSpy>
#include <QTemporaryFile>
#include "custom-network-access-manager.h"
#include "models/page.h"
#include "models/profile.h"
#include "models/site.h"
#include "models/source.h"
#include "catch.h"
#include "source-helpers.h"


TEST_CASE("Page")
{
	setupSource("Danbooru (2.0)");
	setupSite("Danbooru (2.0)", "danbooru.donmai.us");

	setupSource("Gelbooru (0.2)");
	setupSite("Gelbooru (0.2)", "gelbooru.com");

	const QScopedPointer<Profile> pProfile(makeProfile());
	auto *profile = pProfile.data();

	QList<Site*> sites { profile->getSites().value("danbooru.donmai.us") };
	Site *site = profile->getSites().value("gelbooru.com");

	REQUIRE(site != nullptr);
	REQUIRE(sites[0] != nullptr);

	SECTION("IncompatibleModifiers")
	{
		Page page(profile, site, sites, QStringList() << "test" << "status:deleted");

		REQUIRE(page.search().count() == 1);
		REQUIRE(page.search().first() == QString("test"));
	}

	SECTION("LoadAbort")
	{
		Page page(profile, site, sites, QStringList() << "test" << "status:deleted");

		QSignalSpy spy(&page, SIGNAL(finishedLoading(Page*)));
		page.load();
		page.abort();
		REQUIRE(!spy.wait(1000));
	}

	SECTION("LoadTagsAbort")
	{
		Page page(profile, site, sites, QStringList() << "test" << "status:deleted");

		QSignalSpy spy(&page, SIGNAL(finishedLoadingTags(Page*)));
		page.loadTags();
		page.abortTags();
		REQUIRE(!spy.wait(1000));
	}
}

TEST_CASE("A rejected search stops the API fallback")
{
	setupSource("Danbooru (2.0)");
	setupSite("Danbooru (2.0)", "danbooru.donmai.us");
	const QScopedPointer<Profile> profile(makeProfile());
	auto *site = profile->getSites().value("danbooru.donmai.us");
	REQUIRE(site != nullptr);
	site->setAutoLogin(false);

	QTemporaryFile rejected;
	REQUIRE(rejected.open());
	REQUIRE(rejected.write(R"(<?xml version="1.0" encoding="UTF-8"?><result><success type="boolean">false</success><error>PostQuery::TagLimitError</error><message>You cannot search for more than 2 tags at a time.</message></result>)") > 0);
	rejected.flush();

	CustomNetworkAccessManager::NextFiles.clear();
	CustomNetworkAccessManager::NextFiles.enqueue(rejected.fileName());
	// Would only be consumed if the site needlessly fell back to its other APIs.
	CustomNetworkAccessManager::NextFiles.enqueue(rejected.fileName());

	Page page(profile.data(), site, {site}, QStringList{"1girl", "solo", "smile"});
	QSignalSpy failed(&page, &Page::failedLoading);
	page.load();
	REQUIRE(failed.wait());
	REQUIRE(page.errors().join(" ").contains("more than 2 tags"));
	REQUIRE(CustomNetworkAccessManager::NextFiles.size() == 1);
	CustomNetworkAccessManager::NextFiles.clear();
}

TEST_CASE("A site that answers with an empty page is not a failure")
{
	setupSource("Danbooru (2.0)");
	setupSite("Danbooru (2.0)", "danbooru.donmai.us");
	const QScopedPointer<Profile> profile(makeProfile());
	auto *site = profile->getSites().value("danbooru.donmai.us");
	REQUIRE(site != nullptr);
	site->setAutoLogin(false);

	QTemporaryFile xml;
	REQUIRE(xml.open());
	REQUIRE(xml.write(R"(<?xml version="1.0" encoding="UTF-8"?><posts type="array"></posts>)") > 0);
	xml.flush();
	QTemporaryFile json;
	REQUIRE(json.open());
	REQUIRE(json.write(R"([])") > 0);
	json.flush();
	QTemporaryFile html;
	REQUIRE(html.open());
	REQUIRE(html.write(R"(<html><body><p>Nobody here but us chickens!</p></body></html>)") > 0);
	html.flush();

	CustomNetworkAccessManager::NextFiles.clear();
	CustomNetworkAccessManager::NextFiles.enqueue(xml.fileName());
	CustomNetworkAccessManager::NextFiles.enqueue(json.fileName());
	CustomNetworkAccessManager::NextFiles.enqueue(html.fileName());

	Page page(profile.data(), site, {site}, QStringList{"painted_egg", "embarrassed"});
	QSignalSpy finished(&page, &Page::finishedLoading);
	QSignalSpy failed(&page, &Page::failedLoading);
	page.load();
	// The site answered every request; having nothing to show is an empty search, not a broken source.
	REQUIRE(finished.wait());
	REQUIRE(failed.count() == 0);
	REQUIRE(page.images().isEmpty());
	CustomNetworkAccessManager::NextFiles.clear();
}

TEST_CASE("Reloading an empty page stays terminal and does not hang")
{
	setupSource("Danbooru (2.0)");
	setupSite("Danbooru (2.0)", "danbooru.donmai.us");
	const QScopedPointer<Profile> profile(makeProfile());
	auto *site = profile->getSites().value("danbooru.donmai.us");
	REQUIRE(site != nullptr);
	site->setAutoLogin(false);

	QTemporaryFile xml;
	REQUIRE(xml.open());
	REQUIRE(xml.write(R"(<?xml version="1.0" encoding="UTF-8"?><posts type="array"></posts>)") > 0);
	xml.flush();
	QTemporaryFile json;
	REQUIRE(json.open());
	REQUIRE(json.write(R"([])") > 0);
	json.flush();
	QTemporaryFile html;
	REQUIRE(html.open());
	REQUIRE(html.write(R"(<html><body></body></html>)") > 0);
	html.flush();

	CustomNetworkAccessManager::NextFiles.clear();
	for (int i = 0; i < 2; ++i) {
		CustomNetworkAccessManager::NextFiles.enqueue(xml.fileName());
		CustomNetworkAccessManager::NextFiles.enqueue(json.fileName());
		CustomNetworkAccessManager::NextFiles.enqueue(html.fileName());
	}

	Page page(profile.data(), site, {site}, QStringList{"painted_egg"});
	QSignalSpy finished(&page, &Page::finishedLoading);
	QSignalSpy failed(&page, &Page::failedLoading);
	page.load();
	REQUIRE(finished.wait());
	// A second load of the same page answers again instead of waiting on a stale state.
	page.clear();
	page.load();
	REQUIRE(finished.wait());
	REQUIRE(failed.count() == 0);
	CustomNetworkAccessManager::NextFiles.clear();
}

TEST_CASE("An empty page followed by a failed fallback is not a confirmed empty search")
{
	setupSource("Danbooru (2.0)");
	setupSite("Danbooru (2.0)", "danbooru.donmai.us");
	const QScopedPointer<Profile> profile(makeProfile());
	auto *site = profile->getSites().value("danbooru.donmai.us");
	REQUIRE(site != nullptr);
	site->setAutoLogin(false);

	QTemporaryFile xml;
	REQUIRE(xml.open());
	REQUIRE(xml.write(R"(<?xml version="1.0" encoding="UTF-8"?><posts type="array"></posts>)") > 0);
	xml.flush();
	// Not JSON at all: the second API fails after the first one answered with nothing.
	QTemporaryFile broken;
	REQUIRE(broken.open());
	REQUIRE(broken.write(R"({ this is not json )") > 0);
	broken.flush();
	QTemporaryFile html;
	REQUIRE(html.open());
	REQUIRE(html.write(R"(<html><body><p>blocked</p></body></html>)") > 0);
	html.flush();

	CustomNetworkAccessManager::NextFiles.clear();
	CustomNetworkAccessManager::NextFiles.enqueue(xml.fileName());
	CustomNetworkAccessManager::NextFiles.enqueue(broken.fileName());
	CustomNetworkAccessManager::NextFiles.enqueue(html.fileName());

	Page page(profile.data(), site, {site}, QStringList{"painted_egg"});
	QSignalSpy finished(&page, &Page::finishedLoading);
	QSignalSpy failed(&page, &Page::failedLoading);
	page.load();
	// One source said nothing and another could not be read, so "no results" would be a claim nobody made.
	if (failed.isEmpty()) {
		REQUIRE(failed.wait());
	}
	REQUIRE(finished.count() == 0);
	REQUIRE(!page.errors().isEmpty());
	CustomNetworkAccessManager::NextFiles.clear();
}

TEST_CASE("The rate-limit budget belongs to the page and resets on a new load")
{
	setupSource("Danbooru (2.0)");
	setupSite("Danbooru (2.0)", "danbooru.donmai.us");
	const QScopedPointer<Profile> profile(makeProfile());
	auto *site = profile->getSites().value("danbooru.donmai.us");
	REQUIRE(site != nullptr);

	Page page(profile.data(), site, {site}, QStringList{"1girl"});
	const qint64 first = page.rateLimitBudget();
	REQUIRE(first > 0);
	REQUIRE(first <= Page::RateLimitBudget);
	// Every fallback API of the site draws on the same budget, so asking again never adds time.
	REQUIRE(page.rateLimitBudget() <= first);
	// A genuinely new load starts the clock again instead of inheriting the old wait.
	page.clear();
	REQUIRE(page.rateLimitBudget() > Page::RateLimitBudget - 5000);
}

TEST_CASE("Page reload emits once and unsupported endpoints remain readable")
{
	setupSource("Danbooru (2.0)");
	setupSite("Danbooru (2.0)", "danbooru.donmai.us");
	const QScopedPointer<Profile> profile(makeProfile());
	auto *site = profile->getSites().value("danbooru.donmai.us");
	REQUIRE(site != nullptr);
	site->setAutoLogin(false);

	SECTION("Repeated load has one completion handler")
	{
		QTemporaryFile response;
		REQUIRE(response.open());
		REQUIRE(response.write(R"(<posts type="array"><post><id type="integer">7</id><md5>0123456789abcdef0123456789abcdef</md5><file-url>https://example.com/forest.png</file-url><tag-string>forest</tag-string></post></posts>)") > 0);
		response.flush();
		Page page(profile.data(), site, {site}, QStringList{"forest"});
		QSignalSpy finished(&page, &Page::finishedLoading);
		for (int attempt = 0; attempt < 3; ++attempt) {
			CustomNetworkAccessManager::NextFiles.enqueue(response.fileName());
			page.load();
			REQUIRE(finished.wait());
			REQUIRE(finished.size() == attempt + 1);
			REQUIRE(page.images().size() == 1);
		}
	}
	SECTION("Endpoint filtering retains the matching API and empty accessors are safe")
	{
		SearchQuery query;
		query.endpoint = "pool_list";
		Page supported(profile.data(), site, {site}, query);
		REQUIRE(supported.isValid());
		REQUIRE(supported.urls().keys() == QStringList{"Json"});
		REQUIRE(supported.url().path() == "/pools.json");

		query.endpoint = "not_a_supported_endpoint";
		Page unsupported(profile.data(), site, {site}, query, 1, 0);
		REQUIRE_FALSE(unsupported.isValid());
		REQUIRE(unsupported.images().isEmpty());
		REQUIRE(unsupported.tags().isEmpty());
		REQUIRE(unsupported.wiki().isEmpty());
		REQUIRE(unsupported.url().isEmpty());
		REQUIRE(unsupported.friendlyUrl().isEmpty());
		REQUIRE(unsupported.urls().isEmpty());
		REQUIRE(unsupported.imagesCount() == -1);
		REQUIRE(unsupported.maxImagesCount() == -1);
		REQUIRE(unsupported.pagesCount() == -1);
		REQUIRE(unsupported.maxPagesCount() == -1);
		REQUIRE(unsupported.imagesPerPage() == 1);
		REQUIRE(unsupported.pageImageCount() == 0);
		REQUIRE(unsupported.filteredImageCount() == 0);
		REQUIRE(unsupported.highLimit() == 0);
		REQUIRE(unsupported.pageInformation().page == 0);
		REQUIRE_FALSE(unsupported.isLoaded());
		REQUIRE_FALSE(unsupported.hasNext());
		QSignalSpy failed(&unsupported, &Page::failedLoading);
		unsupported.load();
		REQUIRE(failed.size() == 1);
	}
}
