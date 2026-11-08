#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include "models/story-store.h"
#include "catch.h"


TEST_CASE("Stories are kept per picture and still there after a restart", "[story]")
{
	QTemporaryDir directory;
	REQUIRE(directory.isValid());

	{
		StoryStore store(directory.path());
		REQUIRE(store.story("a").isEmpty());
		REQUIRE_FALSE(store.contains("a"));
		store.setStory("a", "  Once, at the edge of the water.  ", "model-x");
		store.setStory("b", "Later, in the same room.", "model-y");
		REQUIRE(store.story("a") == "Once, at the edge of the water.");
		REQUIRE(store.model("a") == "model-x");
		REQUIRE(store.count() == 2);
	}

	// A second instance reads what the first wrote, which is what happens after a restart.
	StoryStore reloaded(directory.path());
	REQUIRE(reloaded.contains("a"));
	REQUIRE(reloaded.story("b") == "Later, in the same room.");
	REQUIRE(reloaded.keys().size() == 2);

	reloaded.remove("a");
	REQUIRE_FALSE(reloaded.contains("a"));
	REQUIRE(StoryStore(directory.path()).count() == 1);
}

TEST_CASE("Stories survive a damaged store instead of being overwritten by it", "[story]")
{
	QTemporaryDir directory;
	REQUIRE(directory.isValid());
	const QString path = QDir(directory.path()).filePath("stories.json");

	// Half-written or corrupt JSON: the next save must not replace it with an empty store.
	{
		QFile file(path);
		REQUIRE(file.open(QIODevice::WriteOnly));
		file.write("{\"a\": {\"text\": \"Kept for recovery\"");
	}
	StoryStore store(directory.path());
	REQUIRE(store.count() == 0);
	REQUIRE_FALSE(store.contains("a"));
	store.setStory("b", "Written after the damage.", "model-z");

	const QStringList damaged = QDir(directory.path()).entryList(QStringList {"stories.json.corrupt-*"}, QDir::Files);
	REQUIRE(damaged.size() == 1);
	QFile kept(QDir(directory.path()).filePath(damaged.first()));
	REQUIRE(kept.open(QIODevice::ReadOnly));
	REQUIRE(kept.readAll().startsWith("{\"a\":"));
	REQUIRE(StoryStore(directory.path()).story("b") == "Written after the damage.");

	// No cap quietly drops the oldest story when the store grows.
	{
		QFile big(path);
		REQUIRE(big.open(QIODevice::WriteOnly));
		QJsonObject root;
		for (int i = 0; i < 6005; ++i) {
			root.insert("k" + QString::number(i), QJsonObject {{"text", "story " + QString::number(i)}, {"model", "m"}, {"at", "2026-01-01T00:00:00Z"}});
		}
		big.write(QJsonDocument(root).toJson());
	}
	StoryStore large(directory.path());
	REQUIRE(large.count() == 6005);
	REQUIRE(large.contains("k0"));
	large.setStory("k99999", "One more.", "m");
	REQUIRE(StoryStore(directory.path()).contains("k0"));
	REQUIRE(StoryStore(directory.path()).count() == 6006);
}
