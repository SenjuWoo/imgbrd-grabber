#include "models/site-mirrors.h"
#include "catch.h"


TEST_CASE("Mirrors of one server are searched once", "[models][site-mirrors]")
{
	REQUIRE(SiteMirrors::group("hijiribe.donmai.us") == SiteMirrors::group("danbooru.donmai.us"));
	REQUIRE(SiteMirrors::group("sonohara.donmai.us") == SiteMirrors::group("danbooru.donmai.us"));
	REQUIRE(SiteMirrors::group("rule34.us") == "rule34.us");
	REQUIRE(SiteMirrors::group("gelbooru.com") != SiteMirrors::group("rule34.us"));

	QStringList dropped;
	const QStringList kept = SiteMirrors::unique({"danbooru.donmai.us", "rule34.us", "hijiribe.donmai.us", "sonohara.donmai.us", "gelbooru.com"}, &dropped);
	REQUIRE(kept == QStringList {"danbooru.donmai.us", "rule34.us", "gelbooru.com"});
	REQUIRE(dropped == QStringList {"hijiribe.donmai.us", "sonohara.donmai.us"});

	// The first address of a server wins, whichever it is.
	REQUIRE(SiteMirrors::unique({"sonohara.donmai.us", "danbooru.donmai.us"}) == QStringList {"sonohara.donmai.us"});
	REQUIRE(SiteMirrors::unique({}).isEmpty());
}
