#include <QColor>
#include <QCryptographicHash>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPixmap>
#include <QScopedPointer>
#include <QSettings>
#include <QTemporaryDir>
#include <QTimer>
#include <cmath>
#include "models/image.h"
#include "models/library-image-encoder.h"
#include "models/library-recommendations.h"
#include "models/library-store.h"
#include "models/profile.h"
#include "models/site.h"
#include "tags/tag.h"
#include "catch.h"
#include "source-helpers.h"

namespace
{
	QString savePicture(Profile *profile, const QString &id, const QColor &color = Qt::green)
	{
		Site *site = profile->getSites().value("danbooru.donmai.us");
		REQUIRE(site != nullptr);
		Image image(site, {{"id", id}, {"file_url", "https://test.invalid/" + id + ".png"}}, profile);
		image.setTags({Tag("forest")});
		QPixmap thumbnail(64, 48);
		thumbnail.fill(color);
		image.setPreviewImage(thumbnail);
		const QString key = profile->library()->saveImage(image);
		REQUIRE_FALSE(key.isEmpty());
		REQUIRE(profile->library()->entry(key).tags().contains("forest"));
		return key;
	}

	QJsonArray vectorFixture()
	{
		QJsonArray vector;
		for (int i = 0; i < 512; ++i) {
			vector.append(i == 0 ? 1.0 : 0.0);
		}
		return vector;
	}

	QJsonObject cacheFixture(const LibraryEntry &entry)
	{
		const QString digest = QString::fromLatin1(QCryptographicHash::hash(entry.thumbnail, QCryptographicHash::Sha256).toHex());
		return {{"version", 1}, {"model", LibraryImageEncoder::modelId()}, {"model_sha256", LibraryImageEncoder::modelSha256()},
			{"entries", QJsonObject {{entry.key, QJsonObject {{"thumbnail", digest}, {"vector", vectorFixture()}}}}}};
	}

	void writeCache(const QString &profilePath, const QByteArray &bytes)
	{
		const QString path = LibraryRecommendations::cachePath(profilePath);
		REQUIRE(QDir().mkpath(QFileInfo(path).absolutePath()));
		QFile file(path);
		REQUIRE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
		REQUIRE(file.write(bytes) == bytes.size());
	}

	void writeCache(const QString &profilePath, const QJsonObject &root)
	{
		writeCache(profilePath, QJsonDocument(root).toJson(QJsonDocument::Compact));
	}

	QByteArray fileBytes(const QString &path)
	{
		QFile file(path);
		REQUIRE(file.open(QIODevice::ReadOnly));
		return file.readAll();
	}

	const QDate Day(2026, 10, 5);
}

TEST_CASE("Local AI cache accepts only current model provenance and valid thumbnail vectors", "[library][recommendations][index]")
{
	QTemporaryDir directory;
	REQUIRE(directory.isValid());
	const QScopedPointer<Profile> profile(makeLibraryProfile(directory.path()));
	profile->getSettings()->setValue("recommendations/localEnabled", false);
	const QString key = savePicture(profile.data(), "10");
	auto root = cacheFixture(profile->library()->entry(key));
	auto records = root["entries"].toObject();
	auto record = records[key].toObject();
	auto vector = record["vector"].toArray();
	int expected = 0;
	SECTION("A current valid cache is reused")
	{
		expected = 1;
	}
	SECTION("A different model is rejected")
	{
		root["model"] = "different-model";
	}
	SECTION("A different model checksum is rejected")
	{
		root["model_sha256"] = QString(64, '0');
	}
	SECTION("A newer cache format is rejected")
	{
		root["version"] = 2;
	}
	SECTION("A different thumbnail is rejected")
	{
		record["thumbnail"] = QString(64, '0');
	}
	SECTION("A short vector is rejected")
	{
		vector.removeLast();
	}
	SECTION("A zero vector is rejected")
	{
		vector[0] = 0.0;
	}
	SECTION("An unnormalized vector is rejected")
	{
		vector[0] = 2.0;
	}
	SECTION("A nonnumeric vector is rejected")
	{
		vector[0] = "1";
	}
	SECTION("Float overflow is rejected")
	{
		vector[0] = 1e300;
	}
	record["vector"] = vector;
	records[key] = record;
	root["entries"] = records;
	writeCache(directory.path(), root);
	const auto before = fileBytes(LibraryRecommendations::cachePath(directory.path()));
	{
		LibraryRecommendations recommendations(profile.data());
		REQUIRE_FALSE(recommendations.busy());
		REQUIRE(recommendations.indexedCount() == expected);
	}
	REQUIRE(fileBytes(LibraryRecommendations::cachePath(directory.path())) == before);
}

TEST_CASE("Malformed local AI cache is preserved without claiming any indexed pictures", "[library][recommendations][index]")
{
	QTemporaryDir directory;
	REQUIRE(directory.isValid());
	const QScopedPointer<Profile> profile(makeLibraryProfile(directory.path()));
	profile->getSettings()->setValue("recommendations/localEnabled", false);
	savePicture(profile.data(), "11");
	const QByteArray damaged("not a JSON cache");
	writeCache(directory.path(), damaged);
	{
		LibraryRecommendations recommendations(profile.data());
		REQUIRE(recommendations.indexedCount() == 0);
		REQUIRE_FALSE(recommendations.busy());
	}
	REQUIRE(fileBytes(LibraryRecommendations::cachePath(directory.path())) == damaged);
}

TEST_CASE("Preference changes keep indexed previews and changed previews invalidate them", "[library][recommendations][index]")
{
	QTemporaryDir directory;
	REQUIRE(directory.isValid());
	const QScopedPointer<Profile> profile(makeLibraryProfile(directory.path()));
	profile->getSettings()->setValue("recommendations/localEnabled", false);
	const QString key = savePicture(profile.data(), "12");
	writeCache(directory.path(), cacheFixture(profile->library()->entry(key)));
	LibraryRecommendations recommendations(profile.data());
	REQUIRE(recommendations.indexedCount() == 1);
	REQUIRE(profile->library()->setLiked(key, true));
	REQUIRE(profile->library()->setFavorite(key, true));
	REQUIRE(profile->library()->setNotes(key, "Existing personal note"));
	REQUIRE(recommendations.indexedCount() == 1);
	REQUIRE(savePicture(profile.data(), "12", Qt::blue) == key);
	REQUIRE(recommendations.indexedCount() == 0);
	REQUIRE_FALSE(profile->library()->entry(key).liked);
	REQUIRE(profile->library()->entry(key).favorite);
	REQUIRE(profile->library()->entry(key).notes == "Existing personal note");
}

TEST_CASE("Scoped hidden suggestions and restoring them persist across profile reopen", "[library][recommendations][index]")
{
	QTemporaryDir directory;
	REQUIRE(directory.isValid());
	qint64 collection = 0;
	QString candidate;
	{
		const QScopedPointer<Profile> profile(makeLibraryProfile(directory.path()));
		profile->getSettings()->setValue("recommendations/localEnabled", false);
		const QString seed = savePicture(profile.data(), "20");
		candidate = savePicture(profile.data(), "21");
		collection = profile->library()->createCollection("Landscape");
		REQUIRE(collection > 0);
		REQUIRE(profile->library()->setFavorite(seed, true));
		REQUIRE(profile->library()->addToCollection(seed, collection));
		REQUIRE(profile->library()->setFavorite(seed, true, collection));
		LibraryRecommendations recommendations(profile.data());
		REQUIRE(recommendations.recommendations(0, Day).items.size() == 1);
		REQUIRE(recommendations.recommendations(collection, Day).items.size() == 1);
		recommendations.hide(candidate, collection);
		REQUIRE(recommendations.recommendations(collection, Day).items.isEmpty());
		REQUIRE(recommendations.recommendations(0, Day).items.size() == 1);
		recommendations.hide(candidate, 0);
		profile->getSettings()->sync();
	}
	{
		const QScopedPointer<Profile> profile(makeLibraryProfile(directory.path()));
		LibraryRecommendations recommendations(profile.data());
		REQUIRE(recommendations.recommendations(0, Day).items.isEmpty());
		REQUIRE(recommendations.recommendations(collection, Day).items.isEmpty());
		recommendations.restoreHidden(collection);
		const auto restored = recommendations.recommendations(collection, Day);
		REQUIRE(restored.items.size() == 1);
		REQUIRE(restored.items[0].entry.key == candidate);
		REQUIRE(recommendations.recommendations(0, Day).items.isEmpty());
		profile->getSettings()->sync();
	}
	{
		const QScopedPointer<Profile> profile(makeLibraryProfile(directory.path()));
		LibraryRecommendations recommendations(profile.data());
		REQUIRE(recommendations.recommendations(collection, Day).items.size() == 1);
		REQUIRE(recommendations.recommendations(0, Day).items.isEmpty());
		recommendations.restoreHidden(0);
		REQUIRE(recommendations.recommendations(0, Day).items.size() == 1);
	}
}

TEST_CASE("Current thumbnail fingerprints reject stale vectors after external catalog changes", "[library][recommendations][index]")
{
	QTemporaryDir directory;
	REQUIRE(directory.isValid());
	const QScopedPointer<Profile> profile(makeLibraryProfile(directory.path()));
	profile->getSettings()->setValue("recommendations/localEnabled", false);
	const QString key = savePicture(profile.data(), "30");
	REQUIRE(profile->library()->setFavorite(key, true));
	savePicture(profile.data(), "31");
	writeCache(directory.path(), cacheFixture(profile->library()->entry(key)));
	LibraryRecommendations recommendations(profile.data());
	REQUIRE(recommendations.recommendations(0, Day).visualSeeds == 1);
	// This separate SQLite connection does not emit the Profile store's signals.
	LibraryStore external(directory.filePath("library.sqlite"));
	REQUIRE(external.isReady());
	Site *site = profile->getSites().value("danbooru.donmai.us");
	Image changed(site, {{"id", "30"}, {"file_url", "https://test.invalid/30.png"}}, profile.data());
	changed.setTags({Tag("forest")});
	QPixmap thumbnail(64, 48);
	thumbnail.fill(Qt::blue);
	changed.setPreviewImage(thumbnail);
	REQUIRE(external.saveImage(changed) == key);
	const auto result = recommendations.recommendations(0, Day);
	REQUIRE(result.ratedSeeds == 1);
	REQUIRE(result.visualSeeds == 0);
	REQUIRE(result.items.size() == 1);
	REQUIRE_FALSE(result.items[0].visual);
}

TEST_CASE("Missing model indexing fails promptly without changing catalog or saved cache", "[library][recommendations][index]")
{
	QTemporaryDir directory;
	REQUIRE(directory.isValid());
	const QScopedPointer<Profile> profile(makeLibraryProfile(directory.path()));
	profile->getSettings()->setValue("recommendations/localEnabled", false);
	const QString key = savePicture(profile.data(), "40");
	REQUIRE(profile->library()->setFavorite(key, true));
	REQUIRE(profile->library()->setNotes(key, "Keep this note"));
	writeCache(directory.path(), cacheFixture(profile->library()->entry(key)));
	const auto catalog = fileBytes(directory.filePath("library.sqlite"));
	const auto cache = fileBytes(LibraryRecommendations::cachePath(directory.path()));
	LibraryRecommendations recommendations(profile.data());
	REQUIRE_FALSE(recommendations.modelAvailable());
	QEventLoop loop;
	QTimer timeout;
	timeout.setSingleShot(true);
	QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
	bool finished = false;
	LibraryIndexResult result;
	QObject::connect(&recommendations, &LibraryRecommendations::indexFinished, &loop, [&](const LibraryIndexResult &value) {
		result = value;
		finished = true;
		loop.quit();
	});
	recommendations.startIndexing();
	SECTION("A missing model reports a clear background failure")
	{}
	SECTION("Cancelling a pending missing-model job cleans up")
	{
		recommendations.cancel();
	}
	if (!finished) {
		timeout.start(5000);
		loop.exec();
	}
	REQUIRE(finished);
	REQUIRE_FALSE(recommendations.busy());
	REQUIRE_FALSE(result.error.isEmpty());
	REQUIRE_FALSE(recommendations.status().isEmpty());
	REQUIRE(result.indexed == 0);
	REQUIRE(result.vectors.size() == 1);
	REQUIRE(profile->library()->entry(key).favorite);
	REQUIRE(profile->library()->entry(key).notes == "Keep this note");
	REQUIRE(fileBytes(directory.filePath("library.sqlite")) == catalog);
	REQUIRE(fileBytes(LibraryRecommendations::cachePath(directory.path())) == cache);
}

TEST_CASE("Real local AI retains cancelled work, resumes, reuses and optionally downloads the pinned model", "[library][recommendations][index][local-ai-runtime]")
{
	const QString model = qEnvironmentVariable("GRABBER_TEST_LOCAL_AI_MODEL");
	if (model.isEmpty()) {
		return; // Normal CI remains offline; parent supplies the verified model and runtime for this check.
	}
	auto fileSha = [](const QString &path) {
		QFile file(path);
		REQUIRE(file.open(QIODevice::ReadOnly));
		QCryptographicHash hash(QCryptographicHash::Sha256);
		REQUIRE(hash.addData(&file));
		return QString::fromLatin1(hash.result().toHex());
	};
	REQUIRE(QFileInfo(model).size() == LibraryImageEncoder::modelSize());
	REQUIRE(fileSha(model) == LibraryImageEncoder::modelSha256());
	auto run = [](LibraryRecommendations &service, bool download = false, int milliseconds = 60000) {
		QEventLoop loop;
		QTimer timeout;
		timeout.setSingleShot(true);
		QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
		bool finished = false;
		LibraryIndexResult result;
		QObject::connect(&service, &LibraryRecommendations::indexFinished, &loop, [&](const LibraryIndexResult &value) {
			result = value;
			finished = true;
			loop.quit();
		});
		QObject::connect(&service, &LibraryRecommendations::changed, &loop, [&]() {
			// A failed download has no indexFinished signal. Check after the successful download can start indexing.
			QTimer::singleShot(0, &loop, [&]() {
				if (!finished && !service.busy()) {
					loop.quit();
				}
			});
		});
		timeout.start(milliseconds);
		if (download) {
			service.downloadModel();
		} else {
			service.startIndexing();
		}
		if (!finished) {
			loop.exec();
		}
		INFO("Local AI status: " + service.status().toStdString());
		if (!finished) {
			service.cancel();
		}
		REQUIRE(finished);
		REQUIRE_FALSE(service.busy());
		REQUIRE(result.error.isEmpty());
		REQUIRE(result.errors.isEmpty());
		REQUIRE(result.failed == 0);
		for (const auto &vector : result.vectors) {
			REQUIRE(vector.size() == 512);
			double norm = 0;
			for (float value : vector) {
				REQUIRE(std::isfinite(value));
				norm += double(value) * value;
			}
			REQUIRE(std::abs(norm - 1.0) < 0.01);
		}
		return result;
	};
	QTemporaryDir directory;
	REQUIRE(directory.isValid());
	const QScopedPointer<Profile> profile(makeLibraryProfile(directory.path()));
	profile->getSettings()->setValue("recommendations/localEnabled", false);
	const QString target = LibraryImageEncoder::modelPath(directory.path());
	REQUIRE(QDir().mkpath(QFileInfo(target).absolutePath()));
	REQUIRE(QFile::copy(model, target));
	QString firstKey;
	for (int i = 0; i < 24; ++i) {
		const QString key = savePicture(profile.data(), QString::number(1000 + i), QColor::fromHsv(i * 15, 200, 180));
		if (firstKey.isEmpty()) {
			firstKey = key;
		}
	}
	REQUIRE(profile->library()->setLiked(firstKey, true));
	REQUIRE(profile->library()->setFavorite(firstKey, true));
	REQUIRE(profile->library()->setNotes(firstKey, "Preserve runtime-test preferences"));
	const auto catalog = fileBytes(directory.filePath("library.sqlite"));
	LibraryRecommendations service(profile.data());
	bool requestedCancel = false;
	const auto cancelConnection = QObject::connect(&service, &LibraryRecommendations::progress, &service, [&](int done, int) {
		if (done > 0 && !requestedCancel) {
			requestedCancel = true; service.cancel();
		}
	});
	const auto partial = run(service);
	QObject::disconnect(cancelConnection);
	REQUIRE(requestedCancel);
	REQUIRE(partial.cancelled);
	REQUIRE(partial.total == 24);
	REQUIRE(partial.indexed > 0);
	REQUIRE(partial.indexed < partial.total);
	REQUIRE(service.indexedCount() == partial.indexed);
	REQUIRE(fileBytes(directory.filePath("library.sqlite")) == catalog);
	profile->getSettings()->setValue("recommendations/localEnabled", false);
	{
		LibraryRecommendations reopened(profile.data());
		REQUIRE_FALSE(reopened.busy());
		REQUIRE(reopened.indexedCount() == partial.indexed);
	}
	const auto completed = run(service);
	REQUIRE_FALSE(completed.cancelled);
	REQUIRE(completed.total == 24);
	REQUIRE(completed.indexed == 24 - partial.indexed);
	REQUIRE(completed.reused == partial.indexed);
	REQUIRE(completed.vectors.size() == 24);
	const auto reused = run(service);
	REQUIRE_FALSE(reused.cancelled);
	REQUIRE(reused.indexed == 0);
	REQUIRE(reused.reused == 24);
	REQUIRE(reused.vectors == completed.vectors);
	REQUIRE(fileBytes(directory.filePath("library.sqlite")) == catalog);
	REQUIRE(profile->library()->entry(firstKey).liked);
	REQUIRE(profile->library()->entry(firstKey).favorite);
	REQUIRE(profile->library()->entry(firstKey).notes == "Preserve runtime-test preferences");
	REQUIRE(fileSha(model) == LibraryImageEncoder::modelSha256());
	// Closing the service must save completed inference even without delivering finished.
	REQUIRE(QFile::remove(LibraryRecommendations::cachePath(directory.path())));
	profile->getSettings()->setValue("recommendations/localEnabled", false);
	int shutdownProgress = 0;
	{
		LibraryRecommendations closing(profile.data());
		QEventLoop loop;
		QTimer timeout;
		timeout.setSingleShot(true);
		QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
		QObject::connect(&closing, &LibraryRecommendations::progress, &loop, [&](int done, int) {
			shutdownProgress = done;
			loop.quit();
		});
		timeout.start(60000);
		closing.startIndexing();
		loop.exec();
		REQUIRE(shutdownProgress > 0);
	}
	profile->getSettings()->setValue("recommendations/localEnabled", false);
	{
		LibraryRecommendations reopened(profile.data());
		REQUIRE(reopened.indexedCount() >= shutdownProgress);
		REQUIRE(reopened.indexedCount() < 24);
		const auto resumed = run(reopened);
		REQUIRE(resumed.reused >= shutdownProgress);
		REQUIRE(resumed.vectors.size() == 24);
	}
	REQUIRE(fileBytes(directory.filePath("library.sqlite")) == catalog);

	if (qEnvironmentVariable("GRABBER_TEST_LOCAL_AI_DOWNLOAD") == "1") {
		QTemporaryDir downloadDirectory;
		REQUIRE(downloadDirectory.isValid());
		const QScopedPointer<Profile> downloadProfile(makeLibraryProfile(downloadDirectory.path()));
		downloadProfile->getSettings()->setValue("recommendations/localEnabled", false);
		for (int i = 0; i < 3; ++i) {
			savePicture(downloadProfile.data(), QString::number(2000 + i), QColor::fromHsv(i * 100, 200, 180));
		}
		const auto downloadCatalog = fileBytes(downloadDirectory.filePath("library.sqlite"));
		LibraryRecommendations downloadService(downloadProfile.data());
		REQUIRE_FALSE(downloadService.modelAvailable());
		const auto downloaded = run(downloadService, true, 180000);
		REQUIRE_FALSE(downloaded.cancelled);
		REQUIRE(downloaded.indexed == 3);
		REQUIRE(downloaded.vectors.size() == 3);
		const QString downloadedModel = LibraryImageEncoder::modelPath(downloadDirectory.path());
		REQUIRE(QFileInfo(downloadedModel).size() == LibraryImageEncoder::modelSize());
		REQUIRE(fileSha(downloadedModel) == LibraryImageEncoder::modelSha256());
		REQUIRE(downloadService.indexedCount() == 3);
		REQUIRE(fileBytes(downloadDirectory.filePath("library.sqlite")) == downloadCatalog);
	}
}
