#include <QJsonArray>
#include <QJsonObject>
#include "models/taste-profile.h"
#include "catch.h"

namespace
{
	LibraryEntry rated(const QString &key, const QString &website, const QList<QPair<QString, QString>> &tags, bool favorite = false)
	{
		LibraryEntry entry;
		entry.key = key;
		entry.liked = !favorite;
		entry.favorite = favorite;
		entry.savedAt = "2026-10-01T00:00:00.000Z";
		entry.image.insert("website", website);
		QJsonArray array;
		for (const auto &tag : tags) {
			array.append(QJsonObject {{"text", tag.first}, {"type", tag.second}});
		}
		entry.image.insert("tags", array);
		return entry;
	}

	const QDateTime Now = QDateTime::fromString("2026-10-09T00:00:00.000Z", Qt::ISODateWithMs);
}

TEST_CASE("Taste profile weighs favorites above likes and ignores meta tags", "[discover]")
{
	const QList<LibraryEntry> entries {
		rated("a", "gelbooru.com", {{"forest", "general"}, {"highres", "meta"}, {"1girl", "general"}}, true),
		rated("b", "gelbooru.com", {{"forest", "general"}, {"1girl", "general"}}),
		rated("c", "gelbooru.com", {{"beach", "general"}, {"1girl", "general"}}),
		rated("d", "gelbooru.com", {{"beach", "general"}, {"1girl", "general"}}),
	};
	const auto profile = TasteProfile::build(entries, {}, Now);
	REQUIRE_FALSE(profile.isEmpty());
	REQUIRE(profile.favorites() == 1);
	REQUIRE(profile.likes() == 3);
	REQUIRE(profile.weight("forest") > profile.weight("beach"));
	REQUIRE(profile.weight("highres") == 0);
	REQUIRE(profile.weight("1girl") < profile.weight("beach"));
	REQUIRE(profile.score({"forest", "river"}) > profile.score({"beach", "river"}));
	REQUIRE(profile.score({}) == 0);
	REQUIRE(profile.score({"unrelated"}) == 0);
}

TEST_CASE("Taste profile turns hidden pictures into weak penalties", "[discover]")
{
	const QList<LibraryEntry> entries {
		rated("a", "gelbooru.com", {{"forest", "general"}, {"lake", "general"}}),
		rated("b", "gelbooru.com", {{"forest", "general"}, {"lake", "general"}}),
	};
	const auto neutral = TasteProfile::build(entries, {}, Now);
	const auto disliked = TasteProfile::build(entries, {{"spiders", 3}, {"forest", 3}}, Now);
	REQUIRE(disliked.weight("spiders") < 0);
	// A clear preference survives a few hidden pictures that also had it.
	REQUIRE(disliked.weight("forest") == neutral.weight("forest"));
	REQUIRE(disliked.score({"forest", "spiders"}) < neutral.score({"forest", "spiders"}));
}

TEST_CASE("Taste profile builds searches only from real, safe tags on selected sources", "[discover]")
{
	const QList<LibraryEntry> entries {
		rated("a", "gelbooru.com", {{"forest", "general"}, {"lake", "general"}, {"order:score", "general"}}, true),
		rated("b", "gelbooru.com", {{"forest", "general"}, {"lake", "general"}}),
		rated("c", "e621.net", {{"dragon", "species"}, {"scales", "general"}}),
		rated("d", "e621.net", {{"dragon", "species"}, {"scales", "general"}}),
		rated("e", "danbooru.donmai.us", {{"someartist", "artist"}}, true),
	};
	const auto profile = TasteProfile::build(entries, {}, Now);
	const auto queries = profile.queries({"gelbooru.com", "safebooru.org"}, 10, 7);
	REQUIRE_FALSE(queries.isEmpty());
	QSet<QString> tags;
	for (const auto &query : queries) {
		REQUIRE((query.website == "gelbooru.com" || query.website == "safebooru.org"));
		REQUIRE_FALSE(query.tags.isEmpty());
		REQUIRE(query.tags.size() <= 2);
		REQUIRE_FALSE(query.reason.isEmpty());
		for (const auto &tag : query.tags) {
			REQUIRE_FALSE(tag.contains(':'));
			tags.insert(tag);
		}
	}
	// e621-only general tags are never searched on other boorus, but an artist can be.
	REQUIRE_FALSE(tags.contains("dragon"));
	REQUIRE_FALSE(tags.contains("scales"));
	REQUIRE(tags.contains("forest"));
	REQUIRE(tags.contains("someartist"));
	REQUIRE(profile.queries({"gelbooru.com"}, 10, 7).size() == profile.queries({"gelbooru.com"}, 10, 7).size());
	REQUIRE(profile.queries({}, 4, 1).isEmpty());
	REQUIRE(profile.topTags(1, {"artist"}).first().name == "someartist");
}

TEST_CASE("Empty taste profile discovers fresh posts from each selected source", "[discover]")
{
	const auto profile = TasteProfile::build({}, {}, Now);
	REQUIRE(profile.isEmpty());
	const auto queries = profile.queries({"a.test", "b.test"}, 4, 3);
	REQUIRE(queries.size() == 4);
	QSet<QString> sites;
	for (const auto &query : queries) {
		REQUIRE(query.tags.isEmpty());
		sites.insert(query.website);
	}
	REQUIRE(sites == QSet<QString> {"a.test", "b.test"});
}
