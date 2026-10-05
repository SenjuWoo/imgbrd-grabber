#include <QJsonArray>
#include <algorithm>
#include <cmath>
#include <limits>
#include "models/library-recommender.h"
#include "catch.h"

namespace
{
	LibraryEntry picture(const QString &key, const QStringList &tags = {}, bool liked = false, bool favorite = false)
	{
		LibraryEntry entry;
		entry.key = key;
		entry.image.insert("tags", QJsonArray::fromStringList(tags));
		entry.liked = liked;
		entry.favorite = favorite;
		return entry;
	}

	QStringList keys(const LibraryRecommendationResult &result)
	{
		QStringList values;
		for (const auto &item : result.items) {
			values.append(item.entry.key);
		}
		return values;
	}

	const QDate Day(2026, 10, 5);
}

TEST_CASE("Recommendations isolate collection ratings and explain real shared tags", "[library][recommendations]")
{
	const auto globalFavorite = picture("red-seed", {"red"}, false, true);
	const auto collectionFavorite = picture("blue-seed", {"blue"}, false, true);
	auto candidate = picture("blue-candidate", {" BLUE ", "sky"}, true, true);
	candidate.notes = "Global-only note";
	const auto redCandidate = picture("red-candidate", {"red"});
	const QList<LibraryEntry> pool {globalFavorite, collectionFavorite, candidate, redCandidate};
	const auto collection = LibraryRecommender::rank(pool, {collectionFavorite}, {}, 42, Day);
	REQUIRE(collection.ratedSeeds == 1);
	REQUIRE(collection.tagSeeds == 1);
	REQUIRE(collection.visualSeeds == 0);
	REQUIRE(keys(collection) == QStringList {"blue-candidate"});
	REQUIRE_FALSE(collection.items[0].entry.liked);
	REQUIRE_FALSE(collection.items[0].entry.favorite);
	REQUIRE(collection.items[0].entry.notes.isEmpty());
	REQUIRE(collection.items[0].basedOnKey == "blue-seed");
	REQUIRE(collection.items[0].sharedTags == QStringList {"blue"});
	REQUIRE_FALSE(collection.items[0].visual);
	const auto global = LibraryRecommender::rank(pool, {globalFavorite}, {}, 0, Day);
	REQUIRE(keys(global) == QStringList {"red-candidate"});
}

TEST_CASE("Favorites influence recommendations three times as strongly as likes", "[library][recommendations]")
{
	const auto favorite = picture("favorite", {"forest"}, true, true);
	const auto liked = picture("liked", {"beach"}, true);
	const auto forest = picture("forest", {"forest"});
	const auto beach = picture("beach", {"beach"});
	const auto result = LibraryRecommender::rank({forest, beach, favorite, liked}, {liked, favorite}, {}, 0, Day);
	REQUIRE(keys(result) == QStringList {"forest", "beach"});
	REQUIRE(result.items[0].score == Catch::Approx(0.75));
	REQUIRE(result.items[1].score == Catch::Approx(0.25));
	REQUIRE(result.items[0].basedOnKey == "favorite");
	REQUIRE(result.items[1].basedOnKey == "liked");
}

TEST_CASE("Tagless recommendations use semantic vectors and preserve missing coverage", "[library][recommendations]")
{
	const auto seed = picture("seed", {}, false, true);
	const auto unavailableSeed = picture("unavailable-seed", {}, true);
	const QList<LibraryEntry> pool {seed, unavailableSeed, picture("near"), picture("orthogonal"), picture("opposite"), picture("unknown")};
	const QHash<QString, QVector<float>> embeddings {
		{"seed", {3, 0}}, {"near", {8, 2}}, {"orthogonal", {0, 1}}, {"opposite", {-1, 0}}
	};
	const auto result = LibraryRecommender::rank(pool, {seed, unavailableSeed}, embeddings, 0, Day);
	REQUIRE(result.ratedSeeds == 2);
	REQUIRE(result.visualSeeds == 1);
	REQUIRE(result.tagSeeds == 0);
	REQUIRE(keys(result) == QStringList {"near"});
	REQUIRE(result.items[0].visual);
	REQUIRE(result.items[0].basedOnKey == "seed");
	REQUIRE(result.items[0].sharedTags.isEmpty());
	REQUIRE(std::isfinite(result.items[0].score));
	REQUIRE(result.items[0].score > 0.9);
	REQUIRE(result.items[0].score <= 1);
	REQUIRE(LibraryRecommender::rank(pool, {unavailableSeed}, embeddings, 0, Day).items.isEmpty());
}

TEST_CASE("Daily recommendations are stable independent of input order and saved dates", "[library][recommendations]")
{
	const auto seed = picture("seed", {"landscape"}, false, true);
	QList<LibraryEntry> pool {seed};
	for (int i = 0; i < 30; ++i) {
		auto entry = picture(QString::number(i), {"landscape"});
		entry.savedAt = i % 2 == 0 ? "9999-12-31" : "invalid date";
		pool.append(entry);
	}
	const auto first = LibraryRecommender::rank(pool, {seed}, {}, 2, Day, 8);
	REQUIRE(first.items.size() == 8);
	std::reverse(pool.begin(), pool.end());
	REQUIRE(keys(first) == keys(LibraryRecommender::rank(pool, {seed}, {}, 2, Day, 8)));
	for (auto &entry : pool) {
		entry.savedAt = "1900-01-01";
	}
	REQUIRE(keys(first) == keys(LibraryRecommender::rank(pool, {seed}, {}, 2, Day, 8)));
	bool changed = false;
	for (int day = 1; day <= 7; ++day) {
		changed = changed || keys(first) != keys(LibraryRecommender::rank(pool, {seed}, {}, 2, Day.addDays(day), 8));
	}
	REQUIRE(changed);
	const QString removed = first.items[0].entry.key;
	const QSet<QString> hidden {removed};
	const auto filtered = LibraryRecommender::rank(pool, {seed}, {}, 2, Day, 8, hidden);
	REQUIRE(filtered.items.size() == 8);
	REQUIRE_FALSE(keys(filtered).contains(removed));
	pool.erase(std::remove_if(pool.begin(), pool.end(), [&removed](const LibraryEntry &entry) { return entry.key == removed; }), pool.end());
	REQUIRE(keys(filtered) == keys(LibraryRecommender::rank(pool, {seed}, {}, 2, Day, 8)));
	REQUIRE(LibraryRecommender::rank(pool, {seed}, {}, 2, Day, 0).items.isEmpty());
}

TEST_CASE("Malformed semantic vectors cannot create recommendations or suppress real tags", "[library][recommendations]")
{
	const auto seed = picture("seed", {"forest"}, false, true);
	const auto brokenSeed = picture("broken-seed", {}, true);
	const QList<LibraryEntry> pool {
		seed, brokenSeed, picture("nan"), picture("infinite"), picture("zero"), picture("mismatch"), picture("too-long"),
		picture("tag-fallback", {"forest"}), picture("empty")
	};
	const QHash<QString, QVector<float>> embeddings {
		{"seed", {1, 0}}, {"broken-seed", {std::numeric_limits<float>::quiet_NaN(), 0}},
		{"nan", {std::numeric_limits<float>::quiet_NaN(), 0}}, {"infinite", {std::numeric_limits<float>::infinity(), 0}},
		{"zero", {0, 0}}, {"mismatch", {1, 0, 0}}, {"too-long", QVector<float>(4097, 1)}, {"tag-fallback", {1}}
	};
	const auto result = LibraryRecommender::rank(pool, {seed, brokenSeed}, embeddings, 0, Day);
	REQUIRE(result.ratedSeeds == 2);
	REQUIRE(result.visualSeeds == 1);
	REQUIRE(result.tagSeeds == 1);
	REQUIRE(keys(result) == QStringList {"tag-fallback"});
	REQUIRE_FALSE(result.items[0].visual);
	REQUIRE(result.items[0].score == Catch::Approx(1));
	REQUIRE(result.items[0].sharedTags == QStringList {"forest"});
}
