#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include "models/pixiv-artwork.h"
#include "catch.h"


TEST_CASE("Pixiv artwork IDs come from file names and artwork URLs", "[pixiv]")
{
	REQUIRE(PixivArtwork::idFrom("Z:/Misc/Grab N/149621459_p5.jpg") == qMakePair(QString("149621459"), 5));
	REQUIRE(PixivArtwork::idFrom("149621459_p0_master1200.jpg") == qMakePair(QString("149621459"), 0));
	REQUIRE(PixivArtwork::idFrom("https://www.pixiv.net/en/artworks/149621459") == qMakePair(QString("149621459"), 0));
	REQUIRE(PixivArtwork::idFrom("https://www.pixiv.net/member_illust.php?mode=medium&illust_id=4242424") == qMakePair(QString("4242424"), 0));
	REQUIRE(PixivArtwork::idFrom("0123456789abcdef0123456789abcdef.jpg").first.isEmpty());
	REQUIRE(PixivArtwork::idFrom("holiday_p5.jpg").first.isEmpty());
}

TEST_CASE("Pixiv artwork details give file URLs even for restricted artworks", "[pixiv]")
{
	// Shape of https://www.pixiv.net/ajax/illust/<id> without login for an R-18 artwork: no file URLs, but the author's list keeps a thumbnail.
	const QJsonArray tags {
		QJsonObject {{"tag", "R-18"}},
		QJsonObject {{"tag", "超乳"}, {"translation", QJsonObject {{"en", "gigantic breasts"}}}},
		QJsonObject {{"tag", "hairy_slut"}},
	};
	const QJsonObject thumbnails {
		{"149621459", QJsonObject {{"url", "https://i.pximg.net/c/250x250_80_a2/custom-thumb/img/2026/09/13/17/32/13/149621459_p0_custom1200.jpg"}}},
		{"149000000", QJsonObject {{"url", "https://i.pximg.net/c/250x250_80_a2/img-master/img/2026/01/01/00/00/00/149000000_p0_square1200.jpg"}}},
	};
	QJsonObject body;
	body.insert("illustId", "149621459");
	body.insert("illustTitle", "Title");
	body.insert("userName", "Some Artist");
	body.insert("userId", "106899176");
	body.insert("pageCount", 13);
	body.insert("width", 1764);
	body.insert("height", 2508);
	body.insert("xRestrict", 1);
	body.insert("urls", QJsonObject {{"mini", QJsonValue()}, {"original", QJsonValue()}});
	body.insert("tags", QJsonObject {{"tags", tags}});
	body.insert("userIllusts", thumbnails);
	const QJsonObject restricted {{"error", false}, {"message", ""}, {"body", body}};
	QString error;
	const auto artwork = PixivArtwork::fromDetails(QJsonDocument(restricted).toJson(), "149621459", 5, &error);
	REQUIRE(artwork.isValid());
	REQUIRE(error.isEmpty());
	REQUIRE(artwork.datePath == "2026/09/13/17/32/13");
	REQUIRE(artwork.originalCandidates().first() == QUrl("https://i.pximg.net/img-original/img/2026/09/13/17/32/13/149621459_p5.jpg"));
	REQUIRE(artwork.originalCandidates().size() == 3);
	REQUIRE(artwork.sampleUrl() == QUrl("https://i.pximg.net/img-master/img/2026/09/13/17/32/13/149621459_p5_master1200.jpg"));
	REQUIRE(artwork.pageUrl() == QUrl("https://www.pixiv.net/artworks/149621459"));
	REQUIRE(artwork.tags == QStringList({"R-18", "gigantic_breasts", "hairy_slut"}));
	REQUIRE(artwork.author == "Some Artist");
	REQUIRE(artwork.pageCount == 13);

	QJsonObject other = restricted;
	other.insert("error", true);
	other.insert("message", "Work has been deleted");
	REQUIRE_FALSE(PixivArtwork::fromDetails(QJsonDocument(other).toJson(), "149621459", 0, &error).isValid());
	REQUIRE(error == "Work has been deleted");
	REQUIRE_FALSE(PixivArtwork::fromDetails("not json", "149621459", 0).isValid());
	REQUIRE_FALSE(PixivArtwork::fromDetails(QJsonDocument(restricted).toJson(), "1", 0).isValid()); // A different artwork's answer.
}
