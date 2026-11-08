#include "ai/picture-story.h"
#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSettings>
#include <QTimer>
#include <QUrl>
#include "logger.h"
#include "models/image.h"
#include "models/library-store.h"
#include "models/profile.h"
#include "models/site.h"


namespace
{
	const QString DefaultBaseUrl = QStringLiteral("https://openrouter.ai/api/v1");
	const QString DefaultModel = QStringLiteral("openrouter/auto");
	// Long enough for a slow free model, short enough that a dead server does not hang the dialog.
	constexpr int TimeoutMs = 120000;
	// Models bill by pixels: a 4000 px original is downscaled before it is sent, both for cost and latency.
	constexpr int MaxImageEdge = 1024;
	// A JPEG that is still huge after downscaling is not worth the request; the tags alone make a story.
	constexpr int MaxImageBytes = 1500 * 1024;
	const QString KeyFile = QStringLiteral("story-key.dat");

	QString setting(Profile *profile, const QString &name)
	{
		return profile->getSettings()->value(name).toString().trimmed();
	}

	// Local imports and missing file URLs have nothing a remote model could fetch.
	QString pictureUrl(const Image &image)
	{
		const QUrl url = image.fileUrl();
		if (!url.isValid() || (url.scheme() != QLatin1String("http") && url.scheme() != QLatin1String("https"))) {
			return {};
		}
		return url.toString();
	}

	QString messageText(const QJsonObject &message)
	{
		const auto content = message.value("content");
		if (content.isString()) {
			return content.toString();
		}
		// Some providers answer with parts: [{"type":"text","text":"..."}].
		QStringList parts;
		for (const auto &part : content.toArray()) {
			const auto text = part.toObject().value("text").toString();
			if (!text.isEmpty()) {
				parts.append(text);
			}
		}
		return parts.join('\n');
	}
}

PictureStory::PictureStory(Profile *profile, QObject *parent)
	: QObject(parent), m_profile(profile), m_manager(new QNetworkAccessManager(this))
{
}

PictureStory::~PictureStory()
{
	abort();
}

QString PictureStory::baseUrl(Profile *profile)
{
	const QString value = setting(profile, "Story/baseUrl");
	return value.isEmpty() ? DefaultBaseUrl : value;
}

QString PictureStory::apiKey(Profile *profile)
{
	QFile file(QDir(profile->getPath()).filePath(KeyFile));
	if (file.open(QIODevice::ReadOnly)) {
		const QString key = QString::fromUtf8(file.read(4096)).trimmed();
		if (!key.isEmpty()) {
			return key;
		}
	}

	// Older builds kept the key in the settings, and settings are part of profile backups: move it out once,
	// and only drop the old copy when the new file really holds it.
	const QString legacy = setting(profile, "Story/apiKey");
	if (!legacy.isEmpty() && setApiKey(profile, legacy)) {
		profile->getSettings()->remove("Story/apiKey");
		profile->getSettings()->sync();
	}
	return legacy;
}

bool PictureStory::setApiKey(Profile *profile, const QString &key)
{
	const QString path = QDir(profile->getPath()).filePath(KeyFile);
	if (key.trimmed().isEmpty()) {
		QFile::remove(path);
		return true;
	}
	QFile file(path);
	if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		log(QStringLiteral("Story: could not save the API key to `%1`").arg(path), Logger::Warning);
		return false;
	}
	if (file.write(key.trimmed().toUtf8()) < 0) {
		return false;
	}
	file.close();
	// The file holds a secret: readable by its owner only where the platform allows it.
	QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
	return true;
}

QString PictureStory::model(Profile *profile)
{
	const QString value = setting(profile, "Story/model");
	return value.isEmpty() ? DefaultModel : value;
}

QUrl PictureStory::endpoint(const QString &baseUrl)
{
	QUrl url(baseUrl.trimmed());
	if (!url.isValid() || url.host().isEmpty() || (url.scheme() != QLatin1String("http") && url.scheme() != QLatin1String("https"))) {
		return {};
	}
	// "http://localhost:1234/v1/" and "http://localhost:1234/v1" are the same server.
	QString path = url.path();
	while (path.endsWith('/')) {
		path.chop(1);
	}
	if (!path.endsWith(QLatin1String("/chat/completions"))) {
		path += QLatin1String("/chat/completions");
	}
	url.setPath(path);
	return url;
}

bool PictureStory::isLocal(const QUrl &url)
{
	const QString host = url.host().toLower();
	return host == QLatin1String("localhost") || host == QLatin1String("127.0.0.1") || host == QLatin1String("::1") || host.endsWith(QLatin1String(".localhost"));
}

QString PictureStory::settingsProblem(Profile *profile)
{
	const QUrl url = endpoint(baseUrl(profile));
	if (!url.isValid() || url.host().isEmpty()) {
		return tr("The AI base URL is not a usable http(s) address.");
	}
	if (apiKey(profile).isEmpty() && !isLocal(url)) {
		return tr("Set an API key for this provider, or point the base URL at a local server like LM Studio.");
	}
	// A key is never sent to a remote host over plain HTTP.
	if (url.scheme() == QLatin1String("http") && !isLocal(url)) {
		return tr("The AI base URL uses plain http; use https so the API key is not sent in clear text.");
	}
	return {};
}

bool PictureStory::configured(Profile *profile)
{
	return settingsProblem(profile).isEmpty();
}

void PictureStory::configure(Profile *profile, const QString &baseUrl, const QString &apiKey, const QString &model)
{
	QSettings *settings = profile->getSettings();
	settings->setValue("Story/baseUrl", baseUrl.trimmed());
	settings->setValue("Story/model", model.trimmed());
	settings->sync();
	setApiKey(profile, apiKey);
}

bool PictureStory::busy() const
{
	return m_reply != nullptr;
}

void PictureStory::abort()
{
	if (m_reply != nullptr) {
		QNetworkReply *reply = m_reply;
		m_reply = nullptr;
		disconnect(reply, nullptr, this, nullptr);
		reply->abort();
		reply->deleteLater();
	}
}

QString PictureStory::imageDataUrl(const QString &path)
{
	// Only the picture the user asked about is read: nothing else from the Library leaves the machine.
	if (path.isEmpty() || !QFileInfo::exists(path) || QFileInfo(path).size() > 32 * 1024 * 1024) {
		return {};
	}
	QImageReader reader(path);
	reader.setAutoTransform(true);
	const QSize size = reader.size();
	if (size.isValid() && !size.isEmpty() && (size.width() > MaxImageEdge || size.height() > MaxImageEdge)) {
		reader.setScaledSize(size.scaled(MaxImageEdge, MaxImageEdge, Qt::KeepAspectRatio));
	}
	const QImage image = reader.read();
	if (image.isNull()) {
		return {};
	}
	QByteArray bytes;
	QBuffer buffer(&bytes);
	if (!buffer.open(QIODevice::WriteOnly) || !image.save(&buffer, "JPG", 85)) {
		return {};
	}
	if (bytes.isEmpty() || bytes.size() > MaxImageBytes) {
		return {};
	}
	return QStringLiteral("data:image/jpeg;base64,") + QString::fromLatin1(bytes.toBase64());
}

void PictureStory::generate(const QSharedPointer<Image> &image, const QString &localPath)
{
	if (image.isNull()) {
		return;
	}
	abort();
	m_retried = false;
	m_image = image;
	m_key = LibraryStore::imageKey(*image);
	if (m_key.isEmpty()) {
		m_key = image->pageUrl().toString();
	}
	m_model = model(m_profile);
	m_pictureUrl = pictureUrl(*image);
	// An imported picture has no public URL, so its own file is sent instead, resized first.
	m_localDataUrl = m_pictureUrl.isEmpty() ? imageDataUrl(localPath) : QString();

	QStringList lines;
	const QStringList tags = image->tagsString();
	if (!tags.isEmpty()) {
		lines.append(QStringLiteral("Tags: ") + tags.mid(0, 80).join(' '));
	}
	if (!image->name().isEmpty()) {
		lines.append(QStringLiteral("Title: ") + image->name());
	}
	if (image->parentSite() != nullptr) {
		lines.append(QStringLiteral("From: ") + image->parentSite()->name());
	}
	const QSize size = image->size();
	if (size.isValid() && !size.isEmpty()) {
		lines.append(QStringLiteral("Size: %1x%2").arg(size.width()).arg(size.height()));
	}
	lines.append(QString());
	lines.append(QStringLiteral("Write the story behind this picture."));
	m_prompt = lines.join('\n');

	const QString problem = settingsProblem(m_profile);
	if (!problem.isEmpty()) {
		emit failed(m_key, problem);
		return;
	}
	send(true);
}

void PictureStory::send(bool withPicture)
{
	const QUrl url = endpoint(baseUrl(m_profile));
	QNetworkRequest request(url);
	request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
	const QString key = apiKey(m_profile);
	if (!key.isEmpty()) {
		request.setRawHeader("Authorization", "Bearer " + key.toUtf8());
	}
	request.setRawHeader("X-Title", "Grabber");
	request.setTransferTimeout(TimeoutMs);

	QJsonArray content;
	content.append(QJsonObject {{"type", "text"}, {"text", m_prompt}});
	// A local import travels as data; a downloaded picture stays a URL the provider fetches itself.
	const QString picture = !m_localDataUrl.isEmpty() ? m_localDataUrl : m_pictureUrl;
	if (withPicture && !picture.isEmpty()) {
		content.append(QJsonObject {
			{"type", "image_url"},
			{"image_url", QJsonObject {{"url", picture}}},
		});
	}
	const QJsonObject body {
		{"model", m_model},
		{"max_tokens", 1000},
		{"messages", QJsonArray {
			QJsonObject {
				{"role", "system"},
				{"content", QStringLiteral("You write the story a picture implies. Close third person or second person, present tense, 150-250 words, concrete sensory detail. No lists, no headings, no framing, no disclaimer, and never mention tags or that you are an AI. When the tags say the picture is explicit, write the adult content as explicitly as the picture suggests; otherwise follow the mood it creates.")},
			},
			QJsonObject {{"role", "user"}, {"content", content}},
		}},
	};

	m_reply = m_manager->post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
	connect(m_reply, &QNetworkReply::finished, this, &PictureStory::finished);
	log(QStringLiteral("Story: asked %1 for %2").arg(m_model, m_key), Logger::Info);
}

void PictureStory::finished()
{
	if (m_reply == nullptr) {
		return;
	}
	QNetworkReply *reply = m_reply;
	m_reply = nullptr;
	const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
	const QByteArray data = reply->readAll();
	const QJsonObject root = QJsonDocument::fromJson(data).object();
	const QString key = m_key;
	reply->deleteLater();

	QString text;
	const auto choices = root.value("choices").toArray();
	if (!choices.isEmpty()) {
		text = messageText(choices.first().toObject().value("message").toObject()).trimmed();
	}
	if (!text.isEmpty()) {
		emit ready(key, text);
		return;
	}

	QString error = root.value("error").toObject().value("message").toString();
	if (error.isEmpty()) {
		error = reply->errorString();
	}
	// A provider that cannot take the picture answers 400/404/415/422; the tags alone still make a story.
	const bool pictureRefused = !m_retried && (!m_pictureUrl.isEmpty() || !m_localDataUrl.isEmpty())
		&& (status == 400 || status == 404 || status == 415 || status == 422);
	if (pictureRefused) {
		m_retried = true;
		log(QStringLiteral("Story: %1 refused the picture (%2), retrying from tags").arg(m_model).arg(status), Logger::Warning);
		send(false);
		return;
	}
	if (status == 401 || status == 403) {
		error = tr("The AI provider rejected the API key.");
	}
	log(QStringLiteral("Story: %1 failed (%2): %3").arg(m_model).arg(status).arg(error), Logger::Warning);
	emit failed(key, error.isEmpty() ? tr("The AI provider returned no story.") : error);
}
