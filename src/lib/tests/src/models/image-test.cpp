#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonObject>
#include <QScopedPointer>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryFile>
#include <QTemporaryDir>
#include "custom-network-access-manager.h"
#include "loader/token.h"
#include "models/image.h"
#include "models/image-factory.h"
#include "models/profile.h"
#include "models/site.h"
#include "models/source.h"
#include "tags/tag.h"
#include "catch.h"
#include "source-helpers.h"


TEST_CASE("Image")
{
	setupSource("Danbooru (2.0)");
	setupSite("Danbooru (2.0)", "danbooru.donmai.us");

	// Make tmp dir if not already existing
	QDir tmp("tests/resources/");
	if (!tmp.exists("tmp")) {
		tmp.mkdir("tmp");
	}

	QFile::remove("tests/resources/md5s.txt");

	QMap<QString, QString> details;
	details["md5"] = "1bc29b36f623ba82aaf6724fd3b16718";
	details["ext"] = "jpg";
	details["author"] = "superauthor";
	details["status"] = "tested";
	details["filename"] = "";
	details["folder"] = "";
	details["search"] = "testing well";
	details["id"] = "7331";
	details["score"] = "21";
	details["parent_id"] = "1337";
	details["file_size"] = "1234567";
	details["creator_id"] = "1234";
	details["has_children"] = "true";
	details["has_note"] = "true";
	details["has_comments"] = "true";
	details["file_url"] = "https://test.com/img/oldfilename.jpg?123456";
	details["sample_url"] = "https://test.com/sample/oldfilename.jpg";
	details["preview_url"] = "https://test.com/preview/oldfilename.jpg";
	details["page_url"] = "/posts/7331";
	details["width"] = "800";
	details["height"] = "600";
	details["source"] = "https://google.com/toto/toto.jpg";
	details["tags_general"] = "tag1 tag2 tag3 ";
	details["tags_artist"] = "artist1 ";
	details["tags_copyright"] = "copyright1 copyright2 ";
	details["tags_character"] = "character1 character2 ";
	details["tags_model"] = "model1 ";
	details["created_at"] = "1471513944";
	details["rating"] = "safe";
	details["file_size"] = "358400";
	details["file_size"] = "358400";

	const QScopedPointer<Profile> pProfile(makeProfile());
	auto *profile = pProfile.data();

	auto *settings = profile->getSettings();
	settings->setValue("Coloring/Fonts/artists", ",8.25,-1,5,50,0,0,0,0,0");
	settings->setValue("Coloring/Fonts/copyrights", ",8.25,-1,5,50,0,0,0,0,0");
	settings->setValue("Coloring/Fonts/characters", ",8.25,-1,5,50,0,0,0,0,0");
	settings->setValue("Coloring/Fonts/generals", ",8.25,-1,5,50,0,0,0,0,0");
	settings->setValue("Save/md5Duplicates", "save");
	settings->setValue("Save/md5DuplicatesSameDir", "save");

	Site *site = profile->getSites().value("danbooru.donmai.us");
	REQUIRE(site != nullptr);

	auto img = ImageFactory::build(site, details, profile);

	SECTION("Constructor")
	{
		QSharedPointer<Image> img;

		// Default
		img = QSharedPointer<Image>(new Image());
		REQUIRE(img->url() == QUrl());

		// Without parent site
		img = ImageFactory::build(nullptr, details, profile);
		REQUIRE(static_cast<int>(img->id()) == 0);

		// With a given page URL
		details["page_url"] = "https://test.com/view/7331";
		img = ImageFactory::build(site, details, profile);
		REQUIRE(img->pageUrl().toString() == QString("https://test.com/view/7331"));

		// CreatedAt from ISO time
		details.remove("created_at");
		details["date"] = "2016-08-26T16:26:30+01:00";
		img = ImageFactory::build(site, details, profile);
		REQUIRE(img->createdAt().toString("yyyy-MM-dd HH:mm:ss") == QString("2016-08-26 16:26:30"));
	}

	SECTION("Validity accepts full images without thumbnails and rejects malformed URLs")
	{
		Image original(site, {{"id", "7"}, {"file_url", "https://example.com/original.png"}}, profile);
		REQUIRE(original.url(Image::Size::Thumbnail).isEmpty());
		REQUIRE(original.name().isEmpty());
		REQUIRE(original.isValid());
		original.setUrl(QUrl("https://[broken-host/original.png"));
		REQUIRE_FALSE(original.url().isValid());
		REQUIRE_FALSE(original.isValid());
		original.setUrl(QUrl());
		REQUIRE_FALSE(original.isValid());
		original.setUrl(QUrl::fromLocalFile(QDir::tempPath() + "/local picture.png"));
		REQUIRE(original.isValid());

		Image thumbnail(site, {{"preview_url", "https://example.com/preview.png"}}, profile);
		REQUIRE(thumbnail.isValid());
		Image namedGallery(site, {{"type", "gallery"}, {"name", "A gallery"}}, profile);
		REQUIRE(namedGallery.isValid());
		Image uninitialized;
		REQUIRE_FALSE(uninitialized.isValid());
	}

	SECTION("Copy")
	{
		Image clone = *img;

		REQUIRE(clone.tokens(profile) == img->tokens(profile));
		REQUIRE(clone.parentSite() == img->parentSite());
		REQUIRE(clone.parentUrl() == img->parentUrl());
	}

	SECTION("HasTag")
	{
		REQUIRE(img->hasTag("tag1"));
		REQUIRE(img->hasTag("character1"));
		REQUIRE(img->hasTag("tag2"));
		REQUIRE(!img->hasTag("tag7"));
		REQUIRE(!img->hasTag("copyright3"));
	}


	/*SECTION("Md5FromFile")
	{
		details.remove("md5");
		img = ImageFactory::build(site, details, profile);
		img->setSavePath("tests/resources/image_1x1.png");

		REQUIRE(img->md5() == QString("956ddde86fb5ce85218b21e2f49e5c50"));
	}*/

	SECTION("Value")
	{
		// Guess from image size
		REQUIRE(img->value() == 800 * 600);

		// Even with a tag, still use image size if possible
		details["tags_general"] = "lowres";
		img = ImageFactory::build(site, details, profile);
		REQUIRE(img->value() == 800 * 600);

		// Default value if nothing is given
		details.remove("width");
		details.remove("height");
		details["tags_general"] = "";
		img = ImageFactory::build(site, details, profile);
		REQUIRE(img->value() == 1200 * 900);

		details["tags_general"] = "incredibly_absurdres";
		img = ImageFactory::build(site, details, profile);
		REQUIRE(img->value() == 10000 * 10000);

		details["tags_general"] = "absurdres";
		img = ImageFactory::build(site, details, profile);
		REQUIRE(img->value() == 3200 * 2400);

		details["tags_general"] = "highres";
		img = ImageFactory::build(site, details, profile);
		REQUIRE(img->value() == 1600 * 1200);

		details["tags_general"] = "lowres";
		img = ImageFactory::build(site, details, profile);
		REQUIRE(img->value() == 500 * 500);
	}

	SECTION("LoadDetails")
	{
		// Load details
		QSignalSpy spy(img.data(), SIGNAL(finishedLoadingTags(Image::LoadTagsResult)));
		img->loadDetails();
		REQUIRE(spy.wait());

		// Compare result
		QList<Tag> tags = img->tags();
		REQUIRE(tags.count() == 26);
		REQUIRE(tags[0].text() == QString("to_heart_2"));
		REQUIRE(tags[0].type().name() == QString("copyright"));
		REQUIRE(tags[0].count() == 6100);
		REQUIRE(tags[1].text() == QString("kousaka_tamaki"));
		REQUIRE(tags[1].type().name() == QString("character"));
		REQUIRE(tags[1].count() == 2100);
		REQUIRE(tags[2].text() == QString("date_(senpen)"));
		REQUIRE(tags[2].type().name() == QString("artist"));
		REQUIRE(tags[2].count() == 256);
		REQUIRE(tags[3].text() == QString("1girl"));
		REQUIRE(tags[3].type().name() == QString("general"));
		REQUIRE(tags[3].count() == 2125000);
	}
	SECTION("LoadDetailsAbort")
	{
		QSignalSpy spy(img.data(), SIGNAL(finishedLoadingTags(Image::LoadTagsResult)));
		img->loadDetails();
		img->abortTags();
		REQUIRE(!spy.wait(1000));
	}

	SECTION("LoadDetailsImageUrl")
	{
		details.remove("file_url");
		img = ImageFactory::build(site, details, profile);

		// Load details
		QSignalSpy spy(img.data(), SIGNAL(finishedLoadingTags(Image::LoadTagsResult)));
		img->loadDetails();
		REQUIRE(spy.wait());

		// Compare result
		REQUIRE(img->url().fileName() == QString("__kousaka_tamaki_to_heart_2_drawn_by_date_senpen__0cc748f006b9636f0c268250ea157995.jpg"));
	}

	SECTION("preSave")
	{
		SECTION("File already saved somewhere else")
		{
			const QString savePath = QDir::toNativeSeparators("tests/resources/tmp/7331.jpg");

			// Delete already existing
			QFile file(savePath);
			if (file.exists()) {
				file.remove();
			}

			img->setSavePath("tests/resources/image_1x1.png");
			Image::SaveResult res = img->preSave(savePath, Image::Size::Full);

			REQUIRE(res == Image::SaveResult::Saved);
			REQUIRE(file.exists());
			file.remove();
		}

		SECTION("Error creating the destination folder")
		{
			QTemporaryFile parentFile;
			REQUIRE(parentFile.open());
			const QString savePath = QDir::toNativeSeparators(parentFile.fileName() + "/7331.jpg");

			img->setSavePath("tests/resources/image_1x1.png");
			Image::SaveResult res = img->preSave(savePath, Image::Size::Full);

			REQUIRE(res == Image::SaveResult::Error);
		}

		SECTION("Destination file already exists on disk")
		{
			const QString savePath = QDir::toNativeSeparators("tests/resources/tmp/7331.jpg");

			// Create file if not exists
			QFile file(savePath);
			REQUIRE(file.open(QFile::Truncate | QFile::WriteOnly));

			img->setSavePath("tests/resources/image_1x1.png");
			Image::SaveResult res = img->preSave(savePath, Image::Size::Full);

			REQUIRE(res == Image::SaveResult::AlreadyExistsDisk);
		}

		SECTION("Failed saves and duplicate operations preserve the source and index")
		{
			QTemporaryDir directory;
			REQUIRE(directory.isValid());
			const QString source = directory.filePath("source.png");
			REQUIRE(QFile::copy("tests/resources/image_1x1.png", source));
			profile->addMd5(img->md5(), source);
			img->setSavePath(source);

			// The parent exists, but this filename exceeds the filesystem limit.
			const QString target = QDir::toNativeSeparators(directory.filePath(QString(300, 'x') + ".png"));
			const QString action = GENERATE(QString("save"), QString("copy"), QString("move"), QString("hardlink"));
			settings->setValue("Save/md5Duplicates", action);
			settings->setValue("Save/md5DuplicatesSameDir", action);
			REQUIRE(img->preSave(target, Image::Size::Full) == Image::SaveResult::Error);
			REQUIRE(QFile::exists(source));
			REQUIRE(!QFile::exists(target));
			REQUIRE(profile->md5Exists(img->md5()).contains(source));
		}

		SECTION("MD5 duplicate actions (ignore, copy, move)")
		{
			const QString savePath = QDir::toNativeSeparators("tests/resources/tmp/7331.jpg");

			// Delete already existing
			QFile file(savePath);
			if (file.exists()) {
				file.remove();
			}

			img->setSavePath("tests/resources/image_1x1.png");
			Image::SaveResult res;

			QFile("tests/resources/image_1x1.png").copy("tests/resources/tmp/source.png");
			profile->addMd5(img->md5(), "tests/resources/tmp/source.png");

			settings->setValue("Save/md5Duplicates", "ignore");
			settings->setValue("Save/md5DuplicatesSameDir", "ignore");
			res = img->preSave(savePath, Image::Size::Full);
			REQUIRE(res == Image::SaveResult::AlreadyExistsMd5);
			REQUIRE(!file.exists());

			settings->setValue("Save/md5Duplicates", "copy");
			settings->setValue("Save/md5DuplicatesSameDir", "copy");
			res = img->preSave(savePath, Image::Size::Full);
			REQUIRE(res == Image::SaveResult::Copied);
			REQUIRE(file.exists());
			REQUIRE(QFile("tests/resources/tmp/source.png").exists());
			file.remove();

			settings->setValue("Save/md5Duplicates", "move");
			settings->setValue("Save/md5DuplicatesSameDir", "move");
			res = img->preSave(savePath, Image::Size::Full);
			REQUIRE(res == Image::SaveResult::Moved);
			REQUIRE(file.exists());
			REQUIRE(!QFile("tests/resources/tmp/source.png").exists());
			file.remove();
		}
	}

	/*SECTION("SaveLog")
	{
		// Delete already existing
		QFile file("tests/resources/tmp/7331.jpg");
		if (file.exists()) {
			file.remove();
		}
		QFile logFile("tests/resources/tmp/savelog.txt");
		if (logFile.exists()) {
			logFile.remove();
		}

		settings->setValue("LogFiles/0/locationType", 1);
		settings->setValue("LogFiles/0/uniquePath", logFile.fileName());
		settings->setValue("LogFiles/0/content", "id: %id%");

		img->setSavePath("tests/resources/image_1x1.png");
		QMap<QString, Image::SaveResult> res = img->save(QString("%id%.%ext%"), QString("tests/resources/tmp/"));

		REQUIRE(res.count() == 1);
		REQUIRE(res.first() == Image::SaveResult::Saved);
		REQUIRE(file.exists());
		REQUIRE(logFile.exists());

		REQUIRE(logFile.open(QFile::ReadOnly | QFile::Text));
		REQUIRE(QString(logFile.readAll()) == QString("id: 7331"));
		logFile.close();

		file.remove();
		logFile.remove();

		settings->remove("LogFiles/0/locationType");
		settings->remove("LogFiles/0/uniquePath");
		settings->remove("LogFiles/0/content");
	}*/

	SECTION("SetUrl")
	{
		QUrl url("https://google.fr");

		REQUIRE(img->url() != url);
		img->setUrl(url);
		REQUIRE(img->url() == url);
	}

	SECTION("GrabberFavoritedToken")
	{
		auto tokens = img->tokens(profile);
		REQUIRE(!tokens["grabber"].value().toStringList().contains("favorited"));

		Favorite fav("tag2");
		profile->addFavorite(fav);
		img->refreshTokens();
		tokens = img->tokens(profile);
		REQUIRE(tokens["grabber"].value().toStringList().contains("favorited"));
		profile->removeFavorite(fav);
	}

	SECTION("Detached preview coloring preserves the saved search")
	{
		Favorite favorite("tag2");
		profile->addFavorite(favorite);
		QJsonObject json;
		img->write(json);
		Image detached(profile);
		REQUIRE(detached.read(json, profile->getSites()));
		REQUIRE(detached.page() == nullptr);
		REQUIRE(detached.color() == QColor("#ffc0cb"));
		json["search"] = QJsonArray::fromStringList({"tag2"});
		Image exactSearch(profile);
		REQUIRE(exactSearch.read(json, profile->getSites()));
		REQUIRE(exactSearch.color() == QColor(204, 204, 0));
		profile->removeFavorite(favorite);
	}

	SECTION("Serialization")
	{
		QJsonObject json;
		img->write(json);

		Image dest(profile);
		dest.read(json, profile->getSites());

		REQUIRE(dest.id() == img->id());
		REQUIRE(dest.md5() == img->md5());
		REQUIRE(dest.fileSize() == img->fileSize());

		REQUIRE(dest.url() == img->url());
		REQUIRE(dest.url(Image::Size::Sample) == img->url(Image::Size::Sample));
		REQUIRE(dest.url(Image::Size::Thumbnail) == img->url(Image::Size::Thumbnail));
	}

	SECTION("Default tokens")
	{
		details.remove("rating");

		img = ImageFactory::build(site, details, profile);
		const auto tokens = img->tokens(profile);

		REQUIRE(tokens.contains("rating"));
		REQUIRE(tokens["rating"].value().toString() == "unknown");
	}
}

TEST_CASE("A post page refused to anonymous users is read through the site's HTML instead")
{
	setupSource("Gelbooru (0.2)");
	setupSite("Gelbooru (0.2)", "api.rule34.xxx");
	const QScopedPointer<Profile> pProfile(makeProfile());
	Profile *profile = pProfile.data();
	Site *site = profile->getSites().value("api.rule34.xxx");
	REQUIRE(site != nullptr);
	site->setAutoLogin(false);

	// A picture from an HTML listing: its file address is guessed from the thumbnail
	QMap<QString, QString> details;
	details["id"] = "5089291";
	details["md5"] = "e9586f76c9438fafd806e987aedb799b";
	details["file_url"] = "https://wimg.rule34.xxx/images/4468/e9586f76c9438fafd806e987aedb799b.jpg";
	details["preview_url"] = "https://wimg.rule34.xxx/thumbnails/4468/thumbnail_e9586f76c9438fafd806e987aedb799b.jpg";
	details["tags_general"] = "landscape sky";
	auto img = ImageFactory::build(site, details, profile);
	REQUIRE(img->pageUrl().toString().contains("json=1"));

	// What api.rule34.xxx answers without an API key: HTTP 200 and a bare JSON string
	QTemporaryFile refusal;
	REQUIRE(refusal.open());
	REQUIRE(refusal.write(R"("Missing authentication. Go to api.rule34.xxx for more information")") > 0);
	refusal.flush();
	QTemporaryFile post;
	REQUIRE(post.open());
	REQUIRE(post.write(R"(<html><body><ul id="tag-sidebar"><li class="tag-type-general tag"><a href="index.php?page=post&amp;s=list&amp;tags=sky">sky</a> <span class="tag-count">10</span></li></ul>)"
		R"(<img alt="sky" height="1200" id="image" src="https://wimg.rule34.xxx/samples/4468/sample_e9586f76c9438fafd806e987aedb799b.jpg" width="850" />)"
		R"(<li><a href="https://wimg.rule34.xxx//images/4468/e9586f76c9438fafd806e987aedb799b.png" style="font-weight: bold;">Original image</a></li></body></html>)") > 0);
	post.flush();

	CustomNetworkAccessManager::NextFiles.clear();
	CustomNetworkAccessManager::NextFiles.enqueue(refusal.fileName());
	CustomNetworkAccessManager::NextFiles.enqueue(post.fileName());

	QSignalSpy spy(img.data(), SIGNAL(finishedLoadingTags(Image::LoadTagsResult)));
	img->loadDetails();
	REQUIRE(spy.wait(10000));
	REQUIRE(spy.first().first().value<Image::LoadTagsResult>() == Image::LoadTagsResult::Ok);

	// The real file (a PNG, so the guessed .jpg was a 404) and the post page people can open
	REQUIRE(img->url().toString() == QString("https://wimg.rule34.xxx//images/4468/e9586f76c9438fafd806e987aedb799b.png"));
	REQUIRE(img->pageUrl().toString().contains("page=post&s=view&id=5089291"));
	REQUIRE(CustomNetworkAccessManager::NextFiles.isEmpty());

	// Later pictures go straight to the post page
	auto other = ImageFactory::build(site, details, profile);
	REQUIRE(other->pageUrl().toString().contains("page=post&s=view"));
	CustomNetworkAccessManager::NextFiles.clear();
}

TEST_CASE("Details without any API that can read them end instead of waiting forever")
{
	setupSource("Gelbooru (0.2)");
	setupSite("Gelbooru (0.2)", "api.rule34.xxx");
	const QScopedPointer<Profile> pProfile(makeProfile());
	Profile *profile = pProfile.data();
	Site *site = profile->getSites().value("api.rule34.xxx");
	REQUIRE(site != nullptr);
	site->setAutoLogin(false);

	QMap<QString, QString> details;
	details["id"] = "1";
	details["md5"] = "e9586f76c9438fafd806e987aedb799b";
	details["file_url"] = "https://wimg.rule34.xxx/images/1/e9586f76c9438fafd806e987aedb799b.jpg";
	auto img = ImageFactory::build(site, details, profile);

	// Every details API refuses: the request ends with an error, the second API is asked once.
	CustomNetworkAccessManager::NextFiles.clear();
	CustomNetworkAccessManager::NextFiles.enqueue("401");
	CustomNetworkAccessManager::NextFiles.enqueue("401");
	CustomNetworkAccessManager::NextFiles.enqueue("401");
	QSignalSpy spy(img.data(), SIGNAL(finishedLoadingTags(Image::LoadTagsResult)));
	img->loadDetails();
	REQUIRE(spy.wait(10000));
	REQUIRE(spy.first().first().value<Image::LoadTagsResult>() != Image::LoadTagsResult::Ok);
	REQUIRE(CustomNetworkAccessManager::NextFiles.size() == 1);
	CustomNetworkAccessManager::NextFiles.clear();
}
