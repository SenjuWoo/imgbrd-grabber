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
