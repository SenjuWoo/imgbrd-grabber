#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QImage>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSettings>
#include <QSignalSpy>
#include <QScopedPointer>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>
#include <functional>
#include "catch.h"
#include "ai/picture-story.h"
#include "ai/story-models.h"
#include "backup.h"
#include "models/image.h"
#include "models/profile.h"
#include "models/site.h"
#include "models/story-store.h"
#include "source-helpers.h"
#include "tags/tag.h"
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
	const QString keyPath = QDir(directory.path()).filePath("story-key.dat");
	REQUIRE(QFile::exists(keyPath));
	// Written exactly once, with no leftover bytes from an earlier key.
	{
		QFile stored(keyPath);
		REQUIRE(stored.open(QIODevice::ReadOnly));
		REQUIRE(stored.readAll() == QByteArray("secret"));
	}

	// A legacy key in the settings is moved out once and then ignored there.
	profile.getSettings()->remove("Story/apiKey");
	profile.getSettings()->setValue("Story/apiKey", "legacy");
	profile.getSettings()->sync();
	QFile::remove(QDir(directory.path()).filePath("story-key.dat"));
	REQUIRE(PictureStory::apiKey(&profile) == "legacy");
	REQUIRE_FALSE(profile.getSettings()->contains("Story/apiKey"));
	REQUIRE(PictureStory::apiKey(&profile) == "legacy");
}

TEST_CASE("A picture sent to a vision model is resized and keeps its proportions", "[gui][story]")
{
	QTemporaryDir directory;
	REQUIRE(directory.isValid());
	const QString path = QDir(directory.path()).filePath("large.png");
	QImage image(2000, 1000, QImage::Format_RGB32);
	image.fill(Qt::darkGreen);
	REQUIRE(image.save(path));

	const QString dataUrl = PictureStory::imageDataUrl(path);
	REQUIRE(dataUrl.startsWith("data:image/jpeg;base64,"));
	const QImage decoded = QImage::fromData(QByteArray::fromBase64(dataUrl.mid(dataUrl.indexOf(',') + 1).toLatin1()));
	REQUIRE(decoded.size() == QSize(1024, 512));

	// Nothing readable to send is reported as nothing, so the request is never built with an empty image.
	REQUIRE(PictureStory::imageDataUrl(QDir(directory.path()).filePath("missing.png")).isEmpty());
	QFile broken(QDir(directory.path()).filePath("broken.png"));
	REQUIRE(broken.open(QIODevice::WriteOnly));
	broken.write("not an image");
	broken.close();
	REQUIRE(PictureStory::imageDataUrl(broken.fileName()).isEmpty());
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

TEST_CASE("A backup of an old profile drops a key still sitting in settings.ini", "[gui][story]")
{
	QTemporaryDir directory;
	REQUIRE(directory.isValid());
	Profile profile(directory.path());
	profile.getSettings()->setValue("Story/baseUrl", "https://openrouter.ai/api/v1");
	profile.getSettings()->setValue("Story/apiKey", "sk-legacy-value");
	profile.getSettings()->sync();

	// The backup is taken before AI Stories is ever opened, so nothing has migrated the key yet.
	QTemporaryDir backup;
	const QString zip = QDir(backup.path()).filePath("backup.zip");
	REQUIRE(saveBackup(&profile, zip));
	const QString unpacked = QDir(backup.path()).filePath("unpacked");
	REQUIRE(QDir().mkpath(unpacked));
	REQUIRE(unzipFile(zip, unpacked));

	QFile settings(QDir(unpacked).filePath("settings.ini"));
	REQUIRE(settings.open(QIODevice::ReadOnly));
	const QByteArray contents = settings.readAll();
	REQUIRE_FALSE(contents.contains("sk-legacy-value"));
	REQUIRE_FALSE(contents.contains("Story/apiKey"));
	// The rest of the configuration is still there to restore.
	REQUIRE(contents.contains("openrouter.ai"));

	// The live profile keeps working, and opening the story settings moves the key out of settings.ini.
	REQUIRE(PictureStory::apiKey(&profile) == "sk-legacy-value");
	REQUIRE_FALSE(profile.getSettings()->contains("Story/apiKey"));
	REQUIRE(QFile::exists(QDir(directory.path()).filePath("story-key.dat")));
}


namespace
{
	/**
	 * Stands in for the provider: records every request body and answers with whatever the test queues.
	 * Replies are delivered from the event loop, the way a real network answer arrives.
	 */
	class FakeReply : public QNetworkReply
	{
		public:
			FakeReply(const QNetworkRequest &request, int status, const QByteArray &body, QObject *parent)
				: QNetworkReply(parent), m_status(status), m_body(body)
			{
				setRequest(request);
				setUrl(request.url());
				open(QIODevice::ReadOnly);
			}

			void abort() override {}
			qint64 bytesAvailable() const override { return m_body.size() - m_offset + QIODevice::bytesAvailable(); }

			void fire()
			{
				setAttribute(QNetworkRequest::HttpStatusCodeAttribute, m_status);
				if (m_status >= 400) {
					setError(QNetworkReply::ProtocolInvalidOperationError, QStringLiteral("HTTP %1").arg(m_status));
				}
				setFinished(true);
				emit readyRead();
				emit finished();
			}

		protected:
			qint64 readData(char *data, qint64 maxSize) override
			{
				const qint64 count = qMin<qint64>(maxSize, m_body.size() - m_offset);
				if (count <= 0) {
					return -1;
				}
				memcpy(data, m_body.constData() + m_offset, size_t(count));
				m_offset += count;
				return count;
			}

		private:
			int m_status;
			QByteArray m_body;
			qint64 m_offset = 0;
	};

	class FakeNetwork : public QNetworkAccessManager
	{
		public:
			explicit FakeNetwork(QObject *parent = nullptr) : QNetworkAccessManager(parent) {}

			QList<QByteArray> bodies;
			QList<QByteArray> answers;
			int status = 200;

		protected:
			QNetworkReply *createRequest(Operation, const QNetworkRequest &request, QIODevice *outgoing) override
			{
				bodies.append(outgoing != nullptr ? outgoing->readAll() : QByteArray());
				// A failing status answers with an error body: a 429 that also carried a story would be a lie.
				QByteArray answer = status >= 400
					? QByteArray(R"({"error":{"message":"Request refused"}})")
					: QByteArray(R"({"choices":[{"message":{"content":"A story from the picture."}}]})");
				if (!answers.isEmpty()) {
					answer = answers.takeFirst();
				}
				auto *reply = new FakeReply(request, status, answer, this);
				QTimer::singleShot(0, reply, [reply]() { reply->fire(); });
				return reply;
			}
	};

	void spinUntil(const std::function<bool()> &done, int timeout = 5000)
	{
		QElapsedTimer timer;
		timer.start();
		while (!done() && timer.elapsed() < timeout) {
			QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
		}
	}

	Profile *prepareStory(Profile *profile)
	{
		PictureStory::configure(profile, "https://openrouter.test/api/v1", "test-key", "vendor/vision-model");
		PictureStory::setAutoModel(profile, false);
		PictureStory::setQuality(profile, false);
		profile->getSettings()->sync();
		return profile;
	}

	Site *storySite(Profile *profile)
	{
		Site *site = profile->getSites().value("danbooru.donmai.us");
		REQUIRE(site != nullptr);
		return site;
	}

	QSharedPointer<Image> taggedImage(Profile *profile, bool withUrl = true)
	{
		// Built through the real constructor so the picture has a page URL and the sizes the app expects.
		QMap<QString, QString> details {{"id", "1"}, {"md5", QString(32, 'a')}};
		if (withUrl) {
			details.insert("file_url", "https://test.invalid/1.png");
		}
		auto image = QSharedPointer<Image>::create(storySite(profile), details, profile);
		image->setTags({
			Tag("miko", "character"),
			Tag("touhou", "copyright"),
			Tag("standing", "general"),
			Tag("red_dress", "general"),
			Tag("forest", "general"),
		});
		return image;
	}
}

TEST_CASE("A vision request carries the picture, the names and the tags", "[gui][story]")
{
	QTemporaryDir directory;
	REQUIRE(directory.isValid());
	const QScopedPointer<Profile> owned(makeLibraryProfile(directory.path()));
	Profile &profile = *owned;
	prepareStory(&profile);

	const QString path = QDir(directory.path()).filePath("picture.png");
	QImage picture(64, 64, QImage::Format_RGB32);
	picture.fill(Qt::darkBlue);
	REQUIRE(picture.save(path));

	FakeNetwork network;
	PictureStory story(&profile, nullptr, &network);
	QSignalSpy ready(&story, &PictureStory::ready);
	QSignalSpy failed(&story, &PictureStory::failed);
	story.generate(taggedImage(&profile), path);
	spinUntil([&]() { return ready.count() + failed.count() > 0; });

	REQUIRE(failed.count() == 0);
	REQUIRE(ready.count() == 1);
	REQUIRE(network.bodies.size() == 1);
	const QByteArray body = network.bodies.first();
	// The picture itself travels in the request.
	REQUIRE(body.contains("image_url"));
	REQUIRE(body.contains("data:image/jpeg;base64,"));
	// Names cannot be seen, so they are always described.
	REQUIRE(body.contains("Characters: miko"));
	REQUIRE(body.contains("Series: touhou"));
	REQUIRE(body.contains("standing"));
	REQUIRE(body.contains("red_dress"));
	REQUIRE(story.usedPicture());
	REQUIRE_FALSE(story.visionFallback());
	REQUIRE(story.modelUsed() == "vendor/vision-model");
}

TEST_CASE("A picture that cannot be sent is an error until tags only is chosen", "[gui][story]")
{
	QTemporaryDir directory;
	REQUIRE(directory.isValid());
	const QScopedPointer<Profile> owned(makeLibraryProfile(directory.path()));
	Profile &profile = *owned;
	prepareStory(&profile);

	FakeNetwork network;
	PictureStory story(&profile, nullptr, &network);
	QSignalSpy ready(&story, &PictureStory::ready);
	QSignalSpy failed(&story, &PictureStory::failed);

	// Nothing local, no URL: writing from tags is a decision, not a silent fallback.
	story.generate(taggedImage(&profile, false));
	spinUntil([&]() { return failed.count() > 0; });
	REQUIRE(failed.count() == 1);
	REQUIRE(story.errorKind() == PictureStory::Error::NoPicture);
	REQUIRE(network.bodies.isEmpty());

	// Asked for explicitly, the same picture gets a tags-only story, and no image part is sent.
	ready.clear();
	failed.clear();
	story.generate(taggedImage(&profile, false), {}, true);
	spinUntil([&]() { return ready.count() + failed.count() > 0; });
	REQUIRE(failed.count() == 0);
	REQUIRE(ready.count() == 1);
	REQUIRE(network.bodies.size() == 1);
	REQUIRE_FALSE(network.bodies.first().contains("image_url"));
	REQUIRE_FALSE(story.usedPicture());
	REQUIRE(story.visionFallback());
}

TEST_CASE("A refused picture is reported with its own reason and never retried silently", "[gui][story]")
{
	QTemporaryDir directory;
	REQUIRE(directory.isValid());
	const QScopedPointer<Profile> owned(makeLibraryProfile(directory.path()));
	Profile &profile = *owned;
	prepareStory(&profile);

	const QString path = QDir(directory.path()).filePath("picture.png");
	QImage picture(32, 32, QImage::Format_RGB32);
	picture.fill(Qt::darkRed);
	REQUIRE(picture.save(path));

	// A model that does not read images answers 400.
	FakeNetwork network;
	network.status = 400;
	network.answers = {R"({"error":{"message":"Invalid content type: image_url"}})"};
	PictureStory story(&profile, nullptr, &network);
	QSignalSpy failed(&story, &PictureStory::failed);
	story.generate(taggedImage(&profile), path);
	spinUntil([&]() { return failed.count() > 0; });
	REQUIRE(failed.count() == 1);
	REQUIRE(story.errorKind() == PictureStory::Error::ImageRejected);
	REQUIRE(failed.first().at(1).toString().contains("would not accept the picture"));
	// Exactly one request: the tags-only retry is the user's choice now.
	REQUIRE(network.bodies.size() == 1);

	// A wrong model id is told apart from a model that cannot see.
	FakeNetwork missing;
	missing.status = 404;
	missing.answers = {R"({"error":{"message":"No endpoints found for model"}})"};
	PictureStory second(&profile, nullptr, &missing);
	QSignalSpy secondFailed(&second, &PictureStory::failed);
	second.generate(taggedImage(&profile), path);
	spinUntil([&]() { return secondFailed.count() > 0; });
	REQUIRE(second.errorKind() == PictureStory::Error::InvalidModel);
	REQUIRE(secondFailed.first().at(1).toString().contains("not found"));

	// Rate limiting keeps its own meaning too.
	FakeNetwork busy;
	busy.status = 429;
	PictureStory third(&profile, nullptr, &busy);
	QSignalSpy thirdFailed(&third, &PictureStory::failed);
	third.generate(taggedImage(&profile), path);
	spinUntil([&]() { return thirdFailed.count() > 0; });
	REQUIRE(third.errorKind() == PictureStory::Error::RateLimited);
}

TEST_CASE("Quality mode reads the scene first and writes from it", "[gui][story]")
{
	QTemporaryDir directory;
	REQUIRE(directory.isValid());
	const QScopedPointer<Profile> owned(makeLibraryProfile(directory.path()));
	Profile &profile = *owned;
	prepareStory(&profile);
	PictureStory::setQuality(&profile, true);
	PictureStory::setWriterModel(&profile, "vendor/writer");

	const QString path = QDir(directory.path()).filePath("picture.png");
	QImage picture(48, 48, QImage::Format_RGB32);
	picture.fill(Qt::darkGreen);
	REQUIRE(picture.save(path));

	FakeNetwork network;
	network.answers = {
		R"({"choices":[{"message":{"content":"Characters: one woman in a red dress\nActions: standing\nUncertain: face"}}]})",
		R"({"choices":[{"message":{"content":"She stood at the treeline, the red dress catching the light."}}]})",
	};
	PictureStory story(&profile, nullptr, &network);
	QSignalSpy ready(&story, &PictureStory::ready);
	QSignalSpy failed(&story, &PictureStory::failed);
	story.generate(taggedImage(&profile), path);
	spinUntil([&]() { return ready.count() + failed.count() > 0; });

	REQUIRE(failed.count() == 0);
	REQUIRE(network.bodies.size() == 2);
	// First pass: the picture, described factually.
	REQUIRE(network.bodies.at(0).contains("image_url"));
	REQUIRE(network.bodies.at(0).contains("vendor/vision-model"));
	REQUIRE(network.bodies.at(0).contains("Describe only what is visible"));
	// Second pass: the observed scene, no picture, on the chosen writing model.
	REQUIRE_FALSE(network.bodies.at(1).contains("image_url"));
	REQUIRE(network.bodies.at(1).contains("vendor/writer"));
	REQUIRE(network.bodies.at(1).contains("Observed scene"));
	REQUIRE(network.bodies.at(1).contains("one woman in a red dress"));
	REQUIRE(story.usedPicture());
}

TEST_CASE("Story tags keep actions and clothing ahead of the budget", "[gui][story]")
{
	QStringList tags;
	for (int i = 0; i < 70; ++i) {
		tags.append(QStringLiteral("filler_%1").arg(i));
	}
	tags.append(QStringLiteral("standing"));
	tags.append(QStringLiteral("red_dress"));
	tags.append(QStringLiteral("forest"));

	// The three that describe the scene move ahead of the long general list, even though they come last.
	const QStringList ordered = PictureStory::prioritizedTags(tags, 5);
	REQUIRE(ordered.size() == 5);
	REQUIRE(ordered.at(0) == "standing");
	REQUIRE(ordered.at(1) == "red_dress");
	REQUIRE(ordered.at(2) == "forest");

	// A budget still caps the whole list, and duplicates never double up.
	REQUIRE(PictureStory::prioritizedTags(QStringList {"smile", "smile", "forest"}, 2).size() == 2);
}

TEST_CASE("The model list is read for free vision models only", "[gui][story]")
{
	const QByteArray json = R"({"data":[
		{"id":"vendor/text-only:free","pricing":{"prompt":"0","completion":"0"}},
		{"id":"vendor/paid-vision","architecture":{"input_modalities":["text","image"],"output_modalities":["text"]},"pricing":{"prompt":"0.000001","completion":"0.000002"}},
		{"id":"vendor/free-vision","architecture":{"input_modalities":["text","image"],"output_modalities":["text"]},"pricing":{"prompt":"0","completion":"0"}},
		{"id":"vendor/image-out","architecture":{"input_modalities":["text","image"],"output_modalities":["image"]},"pricing":{"prompt":"0","completion":"0"}}
	]})";

	const QList<StoryModel> models = StoryModelCatalog::parse(json);
	REQUIRE(models.size() == 4);
	// Free and vision-capable first: that is the only one automatic selection may take.
	REQUIRE(models.first().id == "vendor/free-vision");
	REQUIRE(models.first().free);
	REQUIRE(models.first().images);
	// A model that only produces images is not a story writer.
	for (const StoryModel &model : models) {
		if (model.id == "vendor/image-out") {
			REQUIRE_FALSE(model.images);
		}
	}
	// Nothing free and capable leaves paid models at the back, never picked automatically.
	REQUIRE(models.last().id == "vendor/paid-vision");
	REQUIRE_FALSE(models.last().free);
}
