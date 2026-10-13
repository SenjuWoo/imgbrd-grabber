#include <QJsonArray>
#include <QJsonObject>
#include "models/library-health.h"
#include "catch.h"

namespace
{
	LibraryEntry entry(const QString &key, const QString &website, const QStringList &tags, const QStringList &paths = {}, bool thumbnail = true, bool liked = true)
	{
		LibraryEntry result;
		result.key = key;
		result.liked = liked;
		result.localPaths = paths;
		if (thumbnail) {
			result.thumbnail = "png";
		}
		result.image.insert("website", website);
		QJsonArray array;
		for (const QString &tag : tags) {
			array.append(QJsonObject {{"text", tag}, {"type", "general"}});
		}
		result.image.insert("tags", array);
		return result;
	}
}

TEST_CASE("Library health lists each kind of problem once with its pictures", "[models][library][health]")
{
	const QList<LibraryEntry> entries {
		entry("fine", "rule34.us", {"a"}),
		entry("gone", "", {"a"}, {"X:/gone.png"}),
		entry("kept", "rule34.us", {"a"}, {"X:/kept.png"}),
		entry("bare", "rule34.us", {}, {}, false),
		entry("old-site", "dead.example", {"a"}),
	};
	const auto exists = [](const QString &path) { return path == "X:/kept.png"; };
	const auto health = LibraryHealth::compute(entries, {"rule34.us"}, {{"fine", "copy"}}, {"bare"}, exists);

	REQUIRE(health.total == 5);
	REQUIRE(health.liked == 5);
	REQUIRE(health.issue("missing-files")->keys == QStringList {"gone"});
	REQUIRE(health.issue("no-thumbnail")->keys == QStringList {"bare"});
	REQUIRE(health.issue("no-tags")->keys == QStringList {"bare"});
	REQUIRE(health.issue("unknown-source")->keys == QStringList {"old-site"});
	REQUIRE(health.issue("duplicates")->keys == QStringList {"copy"}); // the kept picture is not a problem
	REQUIRE(health.issue("not-rated")->keys == QStringList {"bare"});
	REQUIRE(health.issue("metadata-errors") == nullptr);

	SECTION("A healthy Library reports nothing")
	{
		const auto clean = LibraryHealth::compute({entry("fine", "rule34.us", {"a"})}, {"rule34.us"}, {}, {}, exists);
		REQUIRE(clean.total == 1);
		REQUIRE(clean.issues.isEmpty());
	}
}
