#include <QDir>
#include <QFile>
#include <QSettings>
#include <QTemporaryDir>
#include <QUrl>
#include "catch.h"
#include "ai/picture-story.h"
#include "backup.h"
#include "models/profile.h"
#include "models/story-store.h"
#include "utils/zip.h"


// The endpoint is the part most providers disagree about: a trailing slash, a pasted full path, no path.
TEST_CASE("AI story endpoints accept the common OpenAI-compatible servers", "[gui][story]")
{
	REQUIRE(PictureStory::endpoint("https://openrouter.ai/api/v1").toString() == "https://openrouter.ai/api/v1/chat/completions");
	REQUIRE(PictureStory::endpoint("https://openrouter.ai/api/v1/").toString() == "https://openrouter.ai/api/v1/chat/completions");
	REQUIRE(PictureStory::endpoint("  https://openrouter.ai/api/v1//  ").toString() == "https://openrouter.ai/api/v1/chat/completions");
	REQUIRE(PictureStory::endpoint("http://localhost:1234/v1").toString() == "http://localhost:1234/v1/chat/completions");
	// A full endpoint pasted in is not extended a second time.
	REQUIRE(PictureStory::endpoint("http://localhost:1234/v1/chat/completions").toString() == "http://localhost:1234/v1/chat/completions");
	// Anything that is not a usable http(s) address is refused before a request is built.
	REQUIRE(PictureStory::endpoint("openrouter").isEmpty());
	REQUIRE(PictureStory::endpoint("ftp://example.com/v1").isEmpty());
	REQUIRE(PictureStory::endpoint("").isEmpty());
}

TEST_CASE("AI story settings allow a keyless localhost server and keep the key out of settings", "[gui][story]")
{
	QTemporaryDir directory;
	REQUIRE(directory.isValid());
	Profile profile(directory.path());
	QSettings *settings = profile.getSettings();

	// A remote provider without a key says what is missing instead of failing at request time.
	REQUIRE_FALSE(PictureStory::configured(&profile));
	REQUIRE(PictureStory::settingsProblem(&profile).contains("key"));

	// LM Studio, llama.cpp and Ollama need no key on this machine.
	settings->setValue("Story/baseUrl", "http://localhost:1234/v1/");
	settings->sync();
	REQUIRE(PictureStory::configured(&profile));
	REQUIRE(PictureStory::settingsProblem(&profile).isEmpty());
	REQUIRE(PictureStory::isLocal(QUrl("http://127.0.0.1:8080/v1")));
	REQUIRE_FALSE(PictureStory::isLocal(QUrl("https://openrouter.ai/api/v1")));

	// A key must not travel over plain http to a remote host.
	settings->setValue("Story/baseUrl", "http://example.com/v1");
	settings->sync();
	PictureStory::setApiKey(&profile, "secret");
	REQUIRE(PictureStory::settingsProblem(&profile).contains("https"));

	// The key lives in its own file, never in the settings a profile backup writes.
	PictureStory::configure(&profile, "https://openrouter.ai/api/v1", "secret", "openrouter/auto");
	REQUIRE(PictureStory::apiKey(&profile) == "secret");
	REQUIRE_FALSE(profile.getSettings()->contains("Story/apiKey"));
	REQUIRE(QFile::exists(QDir(directory.path()).filePath("story-key.dat")));

	// A legacy key in the settings is moved out once and then ignored there.
	profile.getSettings()->remove("Story/apiKey");
	profile.getSettings()->setValue("Story/apiKey", "legacy");
	profile.getSettings()->sync();
	QFile::remove(QDir(directory.path()).filePath("story-key.dat"));
	REQUIRE(PictureStory::apiKey(&profile) == "legacy");
	REQUIRE_FALSE(profile.getSettings()->contains("Story/apiKey"));
	REQUIRE(PictureStory::apiKey(&profile) == "legacy");
}

TEST_CASE("A profile backup carries the stories but never the AI key", "[gui][story]")
{
	QTemporaryDir directory;
	REQUIRE(directory.isValid());
	Profile profile(directory.path());
	PictureStory::configure(&profile, "https://openrouter.ai/api/v1", "sk-secret-value", "openrouter/auto");
	StoryStore stories(directory.path());
	stories.setStory("picture", "A story that is backed up.", "openrouter/auto");

	QTemporaryDir backup;
	const QString zip = QDir(backup.path()).filePath("backup.zip");
	REQUIRE(saveBackup(&profile, zip));
	const QString unpacked = QDir(backup.path()).filePath("unpacked");
	REQUIRE(QDir().mkpath(unpacked));
	REQUIRE(unzipFile(zip, unpacked));

	REQUIRE(QFile::exists(QDir(unpacked).filePath("stories.json")));
	REQUIRE_FALSE(QFile::exists(QDir(unpacked).filePath("story-key.dat")));
	QFile settings(QDir(unpacked).filePath("settings.ini"));
	REQUIRE(settings.open(QIODevice::ReadOnly));
	REQUIRE_FALSE(settings.readAll().contains("sk-secret-value"));
}
