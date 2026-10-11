#include "models/pixiv-artwork.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>


bool PixivArtwork::isValid() const
{
	return !id.isEmpty() && !datePath.isEmpty();
}

QUrl PixivArtwork::pageUrl() const
{
	return QUrl("https://www.pixiv.net/artworks/" + id);
}

QUrl PixivArtwork::detailsUrl() const
{
	return QUrl("https://www.pixiv.net/ajax/illust/" + id);
}

QList<QUrl> PixivArtwork::originalCandidates() const
{
	QList<QUrl> result;
	if (isValid()) {
		for (const QString &extension : {QStringLiteral("jpg"), QStringLiteral("png"), QStringLiteral("gif")}) {
			result.append(QUrl(QStringLiteral("https://i.pximg.net/img-original/img/%1/%2_p%3.%4").arg(datePath, id).arg(page).arg(extension)));
		}
	}
	return result;
}

QUrl PixivArtwork::sampleUrl() const
{
	return isValid() ? QUrl(QStringLiteral("https://i.pximg.net/img-master/img/%1/%2_p%3_master1200.jpg").arg(datePath, id).arg(page)) : QUrl();
}

QUrl PixivArtwork::thumbnailUrl() const
{
	return isValid() ? QUrl(QStringLiteral("https://i.pximg.net/c/250x250_80_a2/img-master/img/%1/%2_p%3_square1200.jpg").arg(datePath, id).arg(page)) : QUrl();
}

QPair<QString, int> PixivArtwork::idFrom(const QString &nameOrUrl)
{
	static const QRegularExpression url(R"(pixiv\.net/(?:[a-z]{2}/)?(?:artworks/|i/|member_illust\.php\?(?:.*&)?illust_id=)(\d{4,12}))", QRegularExpression::CaseInsensitiveOption);
	static const QRegularExpression file(R"((?:^|[\\/_\s-])(\d{4,12})_p(\d{1,4})(?:_master\d+|_square\d+|_custom\d+)?\.(?:jpe?g|png|gif|webp)$)", QRegularExpression::CaseInsensitiveOption);
	const auto fileMatch = file.match(nameOrUrl);
	if (fileMatch.hasMatch()) {
		return {fileMatch.captured(1), fileMatch.captured(2).toInt()};
	}
	const auto urlMatch = url.match(nameOrUrl);
	if (urlMatch.hasMatch()) {
		return {urlMatch.captured(1), 0};
	}
	return {};
}

PixivArtwork PixivArtwork::fromDetails(const QByteArray &json, const QString &id, int page, QString *error)
{
	PixivArtwork result;
	QJsonParseError parse;
	const auto root = QJsonDocument::fromJson(json, &parse).object();
	const auto body = root.value("body").toObject();
	if (parse.error != QJsonParseError::NoError || root.value("error").toBool() || body.value("illustId").toString() != id) {
		if (error != nullptr) {
			*error = root.value("message").toString().isEmpty() ? QStringLiteral("Pixiv did not return this artwork.") : root.value("message").toString();
		}
		return result;
	}
	result.id = id;
	result.page = page;
	result.pageCount = body.value("pageCount").toInt();
	result.title = body.value("illustTitle").toString();
	result.author = body.value("userName").toString();
	result.authorId = body.value("userId").toString();
	result.firstPageSize = QSize(body.value("width").toInt(), body.value("height").toInt());
	for (const auto &value : body.value("tags").toObject().value("tags").toArray()) {
		const auto tag = value.toObject();
		// Prefer the English translation so tags match the other sources' vocabulary.
		QString text = tag.value("translation").toObject().value("en").toString();
		if (text.isEmpty()) {
			text = tag.value("tag").toString();
		}
		text = text.trimmed().replace(' ', '_');
		if (!text.isEmpty() && !result.tags.contains(text)) {
			result.tags.append(text);
		}
	}
	// Any of its own image URLs carries the upload date; restricted artworks only keep the one in the author's list.
	const QRegularExpression date(R"(/img/(\d{4}/\d{2}/\d{2}/\d{2}/\d{2}/\d{2})/)" + QRegularExpression::escape(id) + "_");
	QStringList urls;
	for (const auto &value : body.value("urls").toObject()) {
		urls.append(value.toString());
	}
	urls.append(body.value("userIllusts").toObject().value(id).toObject().value("url").toString());
	for (const QString &url : urls) {
		const auto match = date.match(url);
		if (match.hasMatch()) {
			result.datePath = match.captured(1);
			break;
		}
	}
	if (result.datePath.isEmpty() && error != nullptr) {
		*error = QStringLiteral("Pixiv did not say where this artwork's files are.");
	}
	return result;
}
