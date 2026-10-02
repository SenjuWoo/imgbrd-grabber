#include <QScopedPointer>
#include <QSignalSpy>
#include <QTemporaryFile>
#include <QUrlQuery>
#include "custom-network-access-manager.h"
#include "models/page.h"
#include "models/profile.h"
#include "models/site.h"
#include "catch.h"
#include "source-helpers.h"

TEST_CASE("Reddit cursors preserve filters through the native page pipeline")
{
	setupSource("Reddit");
	setupSite("Reddit", "www.reddit.com");
	const QScopedPointer<Profile> profile(makeProfile());
	auto *site = profile->getSites().value("www.reddit.com");
	REQUIRE(site != nullptr);
	site->setAutoLogin(false);
	QTemporaryFile response;
	REQUIRE(response.open());
	response.write(R"({"kind":"Listing","data":{"after":"t3_next","children":[{"kind":"t3","data":{"id":"abc123","title":"Forest","url":"https://i.redd.it/forest.png","post_hint":"image","subreddit":"EarthPorn"}}]}})");
	response.flush();
	CustomNetworkAccessManager::NextFiles.enqueue(response.fileName());
	const QStringList tags {"subreddit:EarthPorn", "forest"};
	Page first(profile.data(), site, {site}, tags);
	QSignalSpy finished(&first, &Page::finishedLoading);
	first.load();
	REQUIRE(finished.wait());
	REQUIRE(first.images().size() == 1);
	REQUIRE(first.hasNext());
	const auto info = first.pageInformation();
	REQUIRE(info.nextPage.path() == "/r/EarthPorn/search.json");
	REQUIRE(QUrlQuery(info.nextPage).queryItemValue("after") == "t3_next");
	REQUIRE(QUrlQuery(info.nextPage).queryItemValue("q").contains("forest"));
	Page second(profile.data(), site, {site}, tags, 2);
	second.setLastPage(info);
	REQUIRE(second.url() == info.nextPage);
}
