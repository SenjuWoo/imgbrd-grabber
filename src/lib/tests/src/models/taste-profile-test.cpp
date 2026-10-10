#include <QJsonArray>
#include <QJsonObject>
#include <QFile>
#include <QJsonDocument>
#include <cmath>
#include <iostream>
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

TEST_CASE("Taste profile weighs tags by how much more often they appear in ratings than on their sources", "[discover]")
{
	TagBackground background;
	for (int i = 0; i < 200; ++i) {
		QStringList tags {"1girl"};
		if (i % 5 < 3) {
			tags.append("breasts"); // 60% of posts.
		}
		if (i % 2 == 0) {
			tags.append("male"); // 50%.
		}
		if (i % 5 == 0) {
			tags.append("anthro"); // 20%, never rated.
		}
		if (i == 0 || i == 1) {
			tags.append("vibrator"); // 1%.
		}
		background.add("rule34.xxx", tags);
	}
	REQUIRE(background.posts("rule34.xxx") == 200);
	REQUIRE(background.frequency("breasts") == 0.6);
	REQUIRE(background.frequency("unseen") == 0);

	QList<LibraryEntry> entries;
	for (int i = 0; i < 10; ++i) {
		QList<QPair<QString, QString>> tags {{"breasts", "general"}, {"1girl", "general"}};
		if (i < 4) {
			tags.append(qMakePair(QStringLiteral("vibrator"), QStringLiteral("general")));
		}
		if (i == 0) {
			tags.append(qMakePair(QStringLiteral("male"), QStringLiteral("general")));
		}
		entries.append(rated(QString::number(i), "rule34.xxx", tags));
	}
	// Another site's vocabulary stays unknown until that site is sampled.
	entries.append(rated("x", "unsampled.test", {{"site_vocabulary", "general"}, {"breasts", "general"}}));
	entries.append(rated("y", "unsampled.test", {{"site_vocabulary", "general"}, {"breasts", "general"}}));
	const auto profile = TasteProfile::build(entries, {}, Now, background);
	REQUIRE(profile.topTags(1).first().name == "vibrator");
	REQUIRE(profile.weight("vibrator") == 1.0);
	REQUIRE(profile.weight("breasts") < 0.2);
	REQUIRE(profile.weight("male") < 0); // Rated far less often than it appears.
	REQUIRE(profile.weight("anthro") < 0); // Common, never rated.
	REQUIRE(profile.weight("site_vocabulary") == 0);
	REQUIRE(profile.score({"vibrator", "breasts"}) > profile.score({"breasts", "1girl"}));
	REQUIRE(profile.score({"breasts", "anthro"}) < profile.score({"breasts"}));
	REQUIRE(profile.siteWeights().value("rule34.xxx") > 0);

	int vibrator = 0;
	for (quint32 seed = 0; seed < 50; ++seed) {
		for (const auto &query : profile.queries({"rule34.xxx"}, 1, seed)) {
			vibrator += query.tags.contains("vibrator") ? 1 : 0;
		}
	}
	REQUIRE(vibrator > 40);
}

TEST_CASE("Taste profile searches tags whose pictures were rated more than ignored ones", "[discover]")
{
	const QList<LibraryEntry> entries {
		rated("a", "gelbooru.com", {{"forest", "general"}, {"lake", "general"}}),
		rated("b", "gelbooru.com", {{"forest", "general"}, {"lake", "general"}}),
	};
	const auto profile = TasteProfile::build(entries, {}, Now);
	REQUIRE(profile.weight("forest") == profile.weight("lake"));
	const QHash<QString, DiscoveryFeedback> feedback {{"forest", {120, 0}}, {"lake", {20, 4}}};
	int forest = 0, lake = 0;
	for (quint32 seed = 0; seed < 200; ++seed) {
		const auto queries = profile.queries({"gelbooru.com"}, 1, seed, feedback);
		REQUIRE(queries.size() == 1);
		forest += queries.first().tags.first() == "forest" ? 1 : 0;
		lake += queries.first().tags.first() == "lake" ? 1 : 0;
	}
	REQUIRE(lake > forest * 3);
}

TEST_CASE("Tag background persists, needs enough samples and fades old ones", "[discover]")
{
	TagBackground background;
	for (int i = 0; i < 40; ++i) {
		background.add("a.test", {"Common Tag", "common_tag", i < 10 ? "rare" : "other"});
	}
	REQUIRE(background.posts("a.test") == 40);
	REQUIRE(background.frequency("common_tag") == -1); // Too few posts to trust.
	for (int i = 0; i < 40; ++i) {
		background.add("a.test", {"common_tag"});
		background.add("b.test", {"common_tag", "b_only"});
	}
	REQUIRE(background.frequency("common_tag") == 1.0); // Normalized and counted once per post.
	REQUIRE(background.frequency("rare") == 0.125);
	REQUIRE(background.frequency("b_only") == 0); // b.test is not trusted yet, and a.test never had it.
	REQUIRE(background.frequency("rare", {{"a.test", 1}, {"b.test", 3}}) == -1); // Mostly rated where nothing is sampled yet.
	REQUIRE(background.frequency("rare", {{"a.test", 3}, {"b.test", 1}}) == 0.125);
	REQUIRE(background.common(0.1).keys().size() == 3);
	REQUIRE(background.posts() == 80);

	const auto restored = TagBackground::fromJson(background.toJson());
	REQUIRE(restored.posts("a.test") == 80);
	REQUIRE(restored.posts("b.test") == 40);
	REQUIRE(restored.frequency("rare") == 0.125);
	REQUIRE(TagBackground::fromJson(QJsonObject {{"x.test", QJsonObject {{"posts", -3}}}}).posts() == 0);

	TagBackground large;
	for (int i = 0; i < 4001; ++i) {
		large.add("c.test", {i % 2 == 0 ? "even" : "odd"});
	}
	REQUIRE(large.posts("c.test") < 2100); // Old samples fade.
	REQUIRE(std::abs(large.frequency("even") - 0.5) < 0.01);
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

// Developer aid: GRABBER_DIAG_PROFILE=<copy of a profile folder> lib-tests "[diag]" prints what Discover would search.
TEST_CASE("Taste profile diagnostics for a profile copy", "[.][diag]")
{
	const QString directory = qEnvironmentVariable("GRABBER_DIAG_PROFILE");
	REQUIRE(!directory.isEmpty());
	LibraryStore store(directory + "/library.sqlite");
	QFile state(directory + "/discover.json");
	REQUIRE(state.open(QIODevice::ReadOnly));
	const auto root = QJsonDocument::fromJson(state.readAll()).object();
	const auto background = TagBackground::fromJson(root.value("background").toObject());
	QHash<QString, DiscoveryFeedback> feedback;
	const auto saved = root.value("feedback").toObject();
	for (auto it = saved.constBegin(); it != saved.constEnd(); ++it) {
		feedback.insert(it.key(), {it.value().toArray().at(0).toDouble(), it.value().toArray().at(1).toDouble()});
	}
	const auto taste = TasteProfile::build(store.entries(), {}, QDateTime::currentDateTimeUtc(), background);
	std::cout << "sampled posts: " << background.posts() << "\n";
	for (const auto &tag : taste.topTags(30)) {
		std::cout << "  " << tag.weight << "  " << tag.name.toStdString() << " (" << tag.count << ")\n";
	}
	QHash<QString, int> searches;
	QStringList sources = taste.siteWeights().keys();
	for (quint32 seed = 0; seed < 100; ++seed) {
		for (const auto &query : taste.queries(sources, 4, seed, feedback)) {
			searches[query.tags.join(' ')] += 1;
		}
	}
	QList<QPair<int, QString>> ranked;
	for (auto it = searches.constBegin(); it != searches.constEnd(); ++it) {
		ranked.append({it.value(), it.key()});
	}
	std::sort(ranked.begin(), ranked.end(), [](const auto &left, const auto &right) { return left.first > right.first; });
	std::cout << "distinct searches in 400: " << ranked.size() << "\n";
	for (const auto &entry : ranked.mid(0, 40)) {
		std::cout << "  " << entry.first << "  " << entry.second.toStdString() << "\n";
	}
}
