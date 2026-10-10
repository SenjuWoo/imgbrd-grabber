#ifndef THUMBNAIL_LOADER_H
#define THUMBNAIL_LOADER_H

#include <QList>
#include <QObject>
#include <QSet>
#include <QSharedPointer>
#include <QSize>
#include <QString>
#include <QUrl>


class Image;
class NetworkReply;

/**
 * Downloads and decodes one picture preview with bounded size, redirect checks and
 * fallback to the source's other real preview URLs. On success the preview is stored on the Image.
 */
class ThumbnailLoader : public QObject
{
	Q_OBJECT

	public:
		ThumbnailLoader(QSharedPointer<Image> image, const QSize &bounds, bool smartSize, QObject *parent = nullptr);
		~ThumbnailLoader() override;
		void load();
		void retry();
		void abort();
		bool isLoading() const;
		QString error() const;
		const QSharedPointer<Image> &image() const;

	signals:
		void finished();

	private:
		void reset();
		void fail(const QString &reason);
		void replyFinished();
		void finish();

		QSharedPointer<Image> m_image;
		QSize m_bounds;
		bool m_smartSize;
		NetworkReply *m_reply = nullptr;
		bool m_aborted = false;
		QSet<QString> m_redirectsSeen;
		int m_redirectHops = 0;
		QUrl m_url;
		QList<QUrl> m_fallbackUrls;
		QString m_error;
};

#endif // THUMBNAIL_LOADER_H
