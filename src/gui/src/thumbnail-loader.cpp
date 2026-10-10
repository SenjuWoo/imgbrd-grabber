#include "thumbnail-loader.h"
#include <QBuffer>
#include <QImageReader>
#include <QNetworkRequest>
#include "models/image.h"
#include "models/site.h"
#include "network/network-follow.h"
#include "network/network-reply.h"


namespace
{
	constexpr qint64 MaxPreviewBytes = 16 * 1024 * 1024;
	constexpr int MaxPreviewDimension = 8192;
	constexpr qint64 MaxPreviewPixels = 16 * 1024 * 1024;
}

ThumbnailLoader::ThumbnailLoader(QSharedPointer<Image> image, const QSize &bounds, bool smartSize, QObject *parent)
	: QObject(parent), m_image(std::move(image)), m_bounds(bounds), m_smartSize(smartSize)
{
	reset();
}

ThumbnailLoader::~ThumbnailLoader()
{
	if (m_reply != nullptr) {
		disconnect(m_reply, nullptr, this, nullptr);
		m_reply->abort();
		m_reply->deleteLater();
		m_reply = nullptr;
	}
}

const QSharedPointer<Image> &ThumbnailLoader::image() const { return m_image; }
QString ThumbnailLoader::error() const { return m_error; }
bool ThumbnailLoader::isLoading() const { return m_reply != nullptr; }

void ThumbnailLoader::reset()
{
	m_url = m_smartSize ? m_image->mediaForSize(m_bounds, true).url : m_image->url(Image::Size::Thumbnail);
	m_fallbackUrls.clear();
	for (const auto size : { Image::Size::Thumbnail, Image::Size::Sample }) {
		const QUrl url = m_image->url(size);
		if (url.isValid() && !url.isEmpty() && url != m_url && !m_fallbackUrls.contains(url)) {
			m_fallbackUrls.append(url);
		}
	}
	m_redirectsSeen.clear();
	m_redirectHops = 0;
}

void ThumbnailLoader::retry()
{
	m_aborted = false;
	reset();
	load();
}

void ThumbnailLoader::abort()
{
	m_aborted = true;
	if (m_reply != nullptr && m_reply->isRunning()) {
		m_reply->abort();
	}
}

void ThumbnailLoader::load()
{
	if (m_aborted) {
		return;
	}
	if (!m_image->previewImage().isNull()) {
		finish();
		return;
	}
	Site *site = m_image->parentSite();
	if (!site || !m_url.isValid() || m_url.isEmpty()) {
		fail(tr("No thumbnail URL is available."));
		return;
	}
	if (m_reply != nullptr) {
		disconnect(m_reply, nullptr, this, nullptr);
		m_reply->deleteLater();
	}
	m_reply = site->get(site->fixUrl(m_url.toString()), Site::QueryType::Thumbnail, m_image->parentUrl(), "preview");
	connect(m_reply, &NetworkReply::finished, this, &ThumbnailLoader::replyFinished);
	connect(m_reply, &NetworkReply::downloadProgress, this, [this](qint64 received, qint64 total) {
		if (received > MaxPreviewBytes || total > MaxPreviewBytes) {
			fail(tr("Thumbnail exceeds the 16 MiB preview limit."));
		}
	});
	connect(m_reply, &NetworkReply::readyRead, this, [this]() {
		if (m_reply && m_reply->bytesAvailable() > MaxPreviewBytes) {
			fail(tr("Thumbnail exceeds the 16 MiB preview limit."));
		}
	});
}

void ThumbnailLoader::fail(const QString &reason)
{
	m_error = reason;
	if (m_reply) {
		disconnect(m_reply, nullptr, this, nullptr);
		m_reply->abort();
		m_reply->deleteLater();
		m_reply = nullptr;
	}
	if (!m_aborted && m_image->parentSite() && !m_fallbackUrls.isEmpty()) {
		m_url = m_fallbackUrls.takeFirst();
		m_redirectsSeen.clear();
		m_redirectHops = 0;
		load();
		return;
	}
	finish();
}

void ThumbnailLoader::replyFinished()
{
	if (m_aborted || !m_reply) {
		return;
	}
	if (m_reply->error() == NetworkReply::NetworkError::OperationCanceledError) {
		return;
	}

	const QUrl target = m_reply->attribute(QNetworkRequest::RedirectionTargetAttribute).toUrl();
	if (!target.isEmpty()) {
		const QUrl redirection = m_reply->url().resolved(target);
		QString reason;
		if (NetworkFollow::takeRedirect(m_reply->url(), redirection, &m_redirectsSeen, &m_redirectHops, &reason) == NetworkFollow::Action::Stop) {
			fail(tr("Thumbnail redirect stopped: %1").arg(reason));
			return;
		}
		m_url = redirection;
		load();
		return;
	}
	if (m_reply->error() != NetworkReply::NetworkError::NoError) {
		fail(tr("Thumbnail could not load: %1").arg(m_reply->errorString()));
		return;
	}
	if (m_reply->bytesAvailable() > MaxPreviewBytes) {
		fail(tr("Thumbnail exceeds the 16 MiB preview limit."));
		return;
	}
	QByteArray data = m_reply->readAll();
	if (data.size() > MaxPreviewBytes) {
		fail(tr("Thumbnail exceeds the 16 MiB preview limit."));
		return;
	}
	QBuffer buffer(&data);
	buffer.open(QIODevice::ReadOnly);
	QImageReader reader(&buffer);
	reader.setAutoTransform(true);
	const QSize size = reader.size();
	if (size.isEmpty() || size.width() > MaxPreviewDimension || size.height() > MaxPreviewDimension
		|| static_cast<qint64>(size.width()) * size.height() > MaxPreviewPixels) {
		fail(tr("Thumbnail has invalid or excessive image dimensions."));
		return;
	}
	const QImage pixels = reader.read();
	if (pixels.isNull()) {
		fail(tr("Thumbnail could not be decoded: %1").arg(reader.errorString()));
		return;
	}
	m_error.clear();
	m_image->setPreviewImage(QPixmap::fromImage(pixels));
	finish();
}

void ThumbnailLoader::finish()
{
	if (m_reply != nullptr) {
		disconnect(m_reply, nullptr, this, nullptr);
		m_reply->deleteLater();
		m_reply = nullptr;
	}
	if (!m_aborted) {
		emit finished();
	}
}
