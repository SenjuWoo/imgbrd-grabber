#include "ai/picture-story.h"
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
	return setting(profile, "Story/apiKey");
}

QString PictureStory::model(Profile *profile)
{
	const QString value = setting(profile, "Story/model");
	return value.isEmpty() ? DefaultModel : value;
}

bool PictureStory::configured(Profile *profile)
{
	return !apiKey(profile).isEmpty() && !baseUrl(profile).isEmpty();
}

void PictureStory::configure(Profile *profile, const QString &baseUrl, const QString &apiKey, const QString &model)
{
	QSettings *settings = profile->getSettings();
	settings->setValue("Story/baseUrl", baseUrl.trimmed());
	settings->setValue("Story/apiKey", apiKey.trimmed());
	settings->setValue("Story/model", model.trimmed());
	settings->sync();
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

void PictureStory::generate(const QSharedPointer<Image> &image)
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

	if (!configured(m_profile)) {
		emit failed(m_key, tr("Set the AI base URL and API key first."));
		return;
	}
	send(true);
}

void PictureStory::send(bool withPicture)
{
	const QUrl url(baseUrl(m_profile) + "/chat/completions");
	QNetworkRequest request(url);
	request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
	request.setRawHeader("Authorization", "Bearer " + apiKey(m_profile).toUtf8());
	request.setRawHeader("X-Title", "Grabber");
	request.setTransferTimeout(TimeoutMs);

	QJsonArray content;
	content.append(QJsonObject {{"type", "text"}, {"text", m_prompt}});
	if (withPicture && !m_pictureUrl.isEmpty()) {
		content.append(QJsonObject {
			{"type", "image_url"},
			{"image_url", QJsonObject {{"url", m_pictureUrl}}},
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
	// A provider that cannot fetch the picture answers 400/404/415/422; the tags alone still make a story.
	const bool pictureRefused = !m_retried && !m_pictureUrl.isEmpty()
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
