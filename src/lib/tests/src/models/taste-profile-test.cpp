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
	const auto disliked = TasteProfile::build(entries, {{"spiders", 3}, {"forest", 3}, {"once", 1}, {"la_ke", 4}, {"solo", 5}}, Now);
	REQUIRE(disliked.weight("spiders") < 0);
	// Tags of rated pictures (also under another spelling), generic ones and one-offs are not held against anything.
	REQUIRE(disliked.weight("forest") == neutral.weight("forest"));
	REQUIRE(disliked.weight("la_ke") == 0);
	REQUIRE(disliked.weight("once") == 0);
	REQUIRE(disliked.weight("solo") == 0);
	REQUIRE(disliked.score({"forest", "spiders"}) < neutral.score({"forest", "spiders"}));
	REQUIRE(disliked.avoidedTags(10).size() == 1);
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
	// Rarer in ratings than on the source, or never rated: neither is a dislike.
	REQUIRE(profile.weight("male") == 0);
	REQUIRE(profile.weight("anthro") == 0);
	REQUIRE(profile.avoidedTags(10).isEmpty());
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

TEST_CASE("Taste profile treats synonyms and other spellings as one taste", "[discover]")
{
	TagBackground background;
	for (int i = 0; i < 100; ++i) {
		background.add("danbooru.donmai.us", {"1girl", i % 10 == 0 ? "stealth_sex_toy" : i % 10 == 1 ? "body_paint" : i % 10 == 2 ? "smiles" : "smile"});
		background.add("rule34.xxx", {"1girls", i % 20 == 0 ? "hidden_sex_toy" : i % 20 == 1 ? "bodypaint" : "smile"});
	}
	QList<LibraryEntry> entries;
	for (int i = 0; i < 6; ++i) {
		entries.append(rated(QString::number(i), "rule34.xxx", {{"hidden_sex_toy", "general"}, {"bodypaint", "general"}, {"1girls", "general"}}));
	}
	entries.append(rated("d", "danbooru.donmai.us", {{"1girl", "general"}}));
	const auto profile = TasteProfile::build(entries, {}, Now, background);
	REQUIRE(profile.weight("hidden_sex_toy") > 0);
	REQUIRE(profile.weight("stealth_sex_toy") == profile.weight("hidden_sex_toy"));
	REQUIRE(profile.weight("body_paint") == profile.weight("bodypaint"));
	REQUIRE(profile.equivalents("hidden_sex_toy") == QStringList {"stealth_sex_toy"});
	REQUIRE(profile.equivalents("bodypaint").contains("body_paint"));
	REQUIRE(profile.equivalents("smiles").isEmpty()); // Plurals are other tags ("ball", "balls").
	REQUIRE(profile.equivalents("1girls").contains("1girl"));
	for (const auto &tag : profile.topTags(20)) {
		REQUIRE(tag.name != "stealth_sex_toy"); // Listed once, under the learned name.
	}
	REQUIRE(profile.score({"stealth_sex_toy"}) > 0);
	// Danbooru is searched with its own name for the theme.
	bool searchedThere = false;
	for (quint32 seed = 0; seed < 40; ++seed) {
		for (const auto &query : profile.queries({"danbooru.donmai.us"}, 2, seed)) {
			REQUIRE_FALSE(query.tags.contains("hidden_sex_toy"));
			REQUIRE_FALSE(query.tags.contains("bodypaint"));
			searchedThere = searchedThere || query.tags.contains("stealth_sex_toy") || query.tags.contains("body_paint");
		}
	}
	REQUIRE(searchedThere);

	TasteTuning tuning;
	tuning.tags.insert("stealth_sex_toy", TasteTuning::Never);
	tuning.tags.insert("bodypaint", TasteTuning::Neutral);
	const auto tuned = TasteProfile::build(entries, {}, Now, background, tuning);
	REQUIRE(tuned.blocks({"hidden_sex_toy"}));
	REQUIRE(tuned.weight("body_paint") == 0);
	REQUIRE(tuned.avoidedTags(10).size() == 2); // hidden_sex_toy and stealth_sex_toy; Neutral is not avoided.
	REQUIRE(TasteTuning::fromJson(tuning.toJson()).tags == tuning.tags);
	tuning.seenHours = 24;
	REQUIRE(TasteTuning::fromJson(tuning.toJson()).seenHours == 24);
	REQUIRE(TasteTuning::fromJson({}).seenHours == 48);
	REQUIRE(TasteProfile::isMetaTag("3:4"));
	REQUIRE(TasteProfile::isMetaTag("2d_(artwork)"));
}

TEST_CASE("Tag adapter writes each source's own OR syntax", "[discover]")
{
	const QStringList names {"hidden_sex_toy", "stealth_sex_toy"};
	REQUIRE(TasteProfile::anyOf("Gelbooru (0.2)", "api.rule34.xxx", names, 2) == "( hidden_sex_toy ~ stealth_sex_toy )");
	REQUIRE(TasteProfile::anyOf("Gelbooru (0.2)", "gelbooru.com", names, 2) == "{hidden_sex_toy ~ stealth_sex_toy}");
	REQUIRE(TasteProfile::anyOf("Modified Gelbooru (0.1)", "rule34.us", names, 2) == "{hidden_sex_toy ~ stealth_sex_toy}");
	REQUIRE(TasteProfile::anyOf("E621", "e621.net", names, 2) == "~hidden_sex_toy ~stealth_sex_toy");
	REQUIRE(TasteProfile::anyOf("Danbooru (2.0)", "danbooru.donmai.us", names, 1) == "~hidden_sex_toy ~stealth_sex_toy");
	REQUIRE(TasteProfile::anyOf("Danbooru (2.0)", "danbooru.donmai.us", names, 2).isEmpty()); // Anonymous two-tag limit.
	REQUIRE(TasteProfile::anyOf("Gelbooru (0.2)", "unknown.example", names, 1).isEmpty()); // Unverified hosts get the plain search.
	REQUIRE(TasteProfile::anyOf("Shimmie", "rule34.paheal.net", names, 1).isEmpty());
	REQUIRE(TasteProfile::anyOf("E621", "e621.net", {"solo"}, 1).isEmpty());
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

TEST_CASE("Taste tuning steers searches and scores", "[discover]")
{
	const QList<LibraryEntry> entries {
		rated("a", "gelbooru.com", {{"forest", "general"}, {"river", "general"}}, true),
		rated("b", "gelbooru.com", {{"forest", "general"}, {"river", "general"}}),
		rated("c", "gelbooru.com", {{"beach", "general"}, {"sunset", "general"}}),
	};
	const QStringList sources {"gelbooru.com", "danbooru.donmai.us"};

	SECTION("Neutral tuning changes nothing")
	{
		REQUIRE(TasteTuning().isNeutral());
		const auto plain = TasteProfile::build(entries, {}, Now);
		const auto tuned = TasteProfile::build(entries, {}, Now, {}, TasteTuning());
		REQUIRE(plain.weight("forest") == tuned.weight("forest"));
	}

	SECTION("More, less and never")
	{
		TasteTuning tuning;
		tuning.tags.insert("sunset", TasteTuning::More);
		tuning.tags.insert("forest", TasteTuning::Less);
		tuning.tags.insert("river", TasteTuning::Never);
		tuning.tags.insert("mountains", TasteTuning::More); // Never rated: still searched, on any source.
		const auto profile = TasteProfile::build(entries, {}, Now, {}, tuning);
		REQUIRE(profile.weight("sunset") > profile.weight("beach"));
		REQUIRE(profile.weight("forest") < 0);
		REQUIRE(profile.weight("river") == -1.0);
		REQUIRE(profile.blocks({"sky", "River"}));
		REQUIRE_FALSE(profile.blocks({"sky", "forest"}));
		REQUIRE(profile.score({"sunset", "sky"}) > profile.score({"forest", "sky"}));
		REQUIRE(profile.matches({"forest", "sunset", "beach", "1girl"}, 5) == QStringList({"sunset", "beach"}));
		REQUIRE(profile.avoidedTags(5).first().name == "river");
		QSet<QString> searched;
		for (quint32 seed = 0; seed < 40; ++seed) {
			for (const auto &query : profile.queries(sources, 4, seed)) {
				for (const auto &tag : query.tags) {
					searched.insert(tag);
				}
			}
		}
		REQUIRE(searched.contains("mountains"));
		REQUIRE(searched.contains("sunset"));
		REQUIRE_FALSE(searched.contains("river"));
		REQUIRE_FALSE(searched.contains("forest"));
	}

	SECTION("Today's focus takes most searches, even without ratings")
	{
		TasteTuning tuning;
		tuning.focus = {"umbrella"};
		for (const auto &base : {entries, QList<LibraryEntry>()}) {
			const auto profile = TasteProfile::build(base, {}, Now, {}, tuning);
			const auto queries = profile.queries(sources, 5, 7);
			REQUIRE(queries.size() >= 3);
			int focused = 0;
			for (const auto &query : queries) {
				focused += query.tags.first() == "umbrella" ? 1 : 0;
			}
			REQUIRE(focused >= 3);
			REQUIRE(queries.first().reason.contains("umbrella"));
			REQUIRE(profile.score({"umbrella", "rain"}) > profile.score({"forest", "river"}));
		}
	}

	SECTION("Tuning round-trips through JSON and rejects junk")
	{
		TasteTuning tuning;
		tuning.tags.insert("Sunset", TasteTuning::More);
		tuning.focus = {"Umbrella", "umbrella"};
		tuning.variety = TasteTuning::Adventurous;
		auto json = tuning.toJson();
		auto tags = json.value("tags").toObject();
		tags.insert("bad", 7);
		json.insert("tags", tags);
		const auto loaded = TasteTuning::fromJson(json);
		REQUIRE(loaded.tags == QHash<QString, int>({{"sunset", TasteTuning::More}}));
		REQUIRE(loaded.focus == QStringList({"umbrella"}));
		REQUIRE(loaded.variety == TasteTuning::Adventurous);
		REQUIRE(TasteTuning::fromJson({{"variety", 9}}).variety == TasteTuning::Adventurous);
	}

	SECTION("Familiar searches fewer distinct topics than adventurous")
	{
		QList<LibraryEntry> many;
		for (int i = 0; i < 60; ++i) {
			many.append(rated(QString::number(i), "gelbooru.com", {{"theme_" + QString::number(i % 40), "general"}, {"theme_" + QString::number(i % 7), "general"}}));
		}
		auto distinct = [&](int variety) {
			TasteTuning tuning;
			tuning.variety = variety;
			const auto profile = TasteProfile::build(many, {}, Now, {}, tuning);
			QSet<QString> topics;
			for (quint32 seed = 0; seed < 60; ++seed) {
				for (const auto &query : profile.queries({"gelbooru.com"}, 4, seed)) {
					topics.insert(query.tags.first());
				}
			}
			return topics.size();
		};
		REQUIRE(distinct(TasteTuning::Familiar) < distinct(TasteTuning::Adventurous));
	}
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
	QHash<QString, double> disliked;
	const auto hidden = root.value("disliked").toObject();
	for (auto it = hidden.constBegin(); it != hidden.constEnd(); ++it) {
		disliked.insert(it.key(), it.value().toDouble());
	}
	const auto taste = TasteProfile::build(store.entries(), disliked, QDateTime::currentDateTimeUtc(), background, TasteTuning::fromJson(root.value("tuning").toObject()));
	std::cout << "sampled posts: " << background.posts() << "\n";
	for (const auto &tag : taste.topTags(60)) {
		std::cout << "  " << tag.weight << "  " << tag.name.toStdString() << " (" << tag.count << ")\n";
	}
	std::cout << "avoided:\n";
	for (const auto &tag : taste.avoidedTags(60)) {
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
