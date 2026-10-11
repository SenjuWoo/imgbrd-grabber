#ifndef PIXIV_ARTWORK_H
#define PIXIV_ARTWORK_H

#include <QByteArray>
#include <QList>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QUrl>

/**
 * One page of a Pixiv artwork, read from Pixiv's public artwork endpoint (no login needed).
 * Restricted artworks hide their file URLs there, but their thumbnail still gives the upload date the file URLs are built from.
 */
struct PixivArtwork
{
	QString id;
	int page = 0;
	int pageCount = 0;
	QString title;
	QString author;
	QString authorId;
	QStringList tags;
	QSize firstPageSize;
	QString datePath; // yyyy/MM/dd/hh/mm/ss of the upload

	bool isValid() const;
	QUrl pageUrl() const;
	QUrl detailsUrl() const;
	/** Possible original files of this page; Pixiv does not say the extension without login. */
	QList<QUrl> originalCandidates() const;
	QUrl sampleUrl() const;
	QUrl thumbnailUrl() const;

	/** Artwork ID and page from a Pixiv file name ("123_p4.jpg") or artwork URL; empty ID when there is none. */
	static QPair<QString, int> idFrom(const QString &nameOrUrl);
	static PixivArtwork fromDetails(const QByteArray &json, const QString &id, int page, QString *error = nullptr);
};

#endif // PIXIV_ARTWORK_H
