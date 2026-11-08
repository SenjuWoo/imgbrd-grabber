// Reads the provider's model list so a vision-capable model can be found without guessing its name.
#include "ai/story-models.h"

#include "ai/picture-story.h"
#include "models/profile.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

#include <algorithm>


StoryModelCatalog::StoryModelCatalog(Profile *profile, QObject *parent, QNetworkAccessManager *manager)
	: QObject(parent), m_profile(profile), m_manager(manager != nullptr ? manager : new QNetworkAccessManager(this))
{}

/**
 * Parses an OpenAI-compatible `/models` answer. OpenRouter describes modalities and pricing per model; LM Studio
 * and plain OpenAI-compatible servers often list nothing but an id, which simply means "not known to be vision".
 */
QList<StoryModel> StoryModelCatalog::parse(const QByteArray &json)
{
	QList<StoryModel> models;
	const QJsonDocument document = QJsonDocument::fromJson(json);
	const QJsonArray data = document.object().value("data").toArray();

	for (const QJsonValue &value : data) {
		const QJsonObject object = value.toObject();
		StoryModel model;
		model.id = object.value("id").toString();
		model.name = object.value("name").toString(object.value("id").toString());
		model.context = object.value("context_length").toInt();
		if (model.id.isEmpty()) {
			continue;
		}

		const QJsonObject architecture = object.value("architecture").toObject();
		const QJsonArray inputs = architecture.value("input_modalities").toArray();
		if (!inputs.isEmpty()) {
			for (const QJsonValue &input : inputs) {
				model.images = model.images || input.toString() == QLatin1String("image");
			}
		} else {
			// Older shapes describe the whole route as one string, like "text+image->text".
			const QString modality = architecture.value("modality").toString();
			model.images = modality.contains(QLatin1String("image"));
		}
		// A model that only answers with pictures cannot write a story.
		const QJsonArray outputs = architecture.value("output_modalities").toArray();
		if (!outputs.isEmpty()) {
			bool writesText = false;
			for (const QJsonValue &output : outputs) {
				writesText = writesText || output.toString() == QLatin1String("text");
			}
			model.images = model.images && writesText;
		}

		const QJsonObject pricing = object.value("pricing").toObject();
		if (!pricing.isEmpty()) {
			const QJsonValue prompt = pricing.value("prompt");
			const QJsonValue completion = pricing.value("completion");
			auto isZero = [](const QJsonValue &price) {
				return price.toString() == QLatin1String("0") || price.toDouble(-1.0) == 0.0;
			};
			model.free = isZero(prompt) && isZero(completion);
		}

		models.append(model);
	}

	// Free first, then the ones that can see: the picker offers the cheapest capable model before anything paid.
	std::sort(models.begin(), models.end(), [](const StoryModel &a, const StoryModel &b) {
		if (a.free != b.free) {
			return a.free;
		}
		if (a.images != b.images) {
			return a.images;
		}
		return a.id < b.id;
	});

	return models;
}

void StoryModelCatalog::fetch()
{
	if (m_reply != nullptr) {
		m_reply->abort();
		m_reply = nullptr;
	}

	// The list lives under the same API root as the chat endpoint; a key must not leave over plain http.
	const QString problem = PictureStory::settingsProblem(m_profile);
	if (!problem.isEmpty()) {
		emit failed(problem);
		return;
	}
	const QUrl url = PictureStory::modelsEndpoint(PictureStory::baseUrl(m_profile));
	if (url.isEmpty()) {
		emit failed(tr("The AI base URL is not a usable http(s) address."));
		return;
	}
	QNetworkRequest request(url);
	const QString key = PictureStory::apiKey(m_profile);
	if (!key.isEmpty()) {
		request.setRawHeader("Authorization", "Bearer " + key.toUtf8());
	}
	request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("Grabber"));
	request.setTransferTimeout(15000);

	m_reply = m_manager->get(request);
	QNetworkReply *reply = m_reply;
	connect(reply, &QNetworkReply::finished, this, [this, reply]() {
		if (m_reply == reply) {
			m_reply = nullptr;
		}
		const QByteArray body = reply->readAll();
		const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
		const QNetworkReply::NetworkError error = reply->error();
		const QString transport = reply->errorString();
		reply->deleteLater();

		if (status != 200) {
			emit failed(error == QNetworkReply::NoError
				? tr("The model list could not be read (HTTP %1).").arg(status)
				: tr("The model list could not be read: %1").arg(transport));
			return;
		}
		emit ready(parse(body));
	});
}
