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
