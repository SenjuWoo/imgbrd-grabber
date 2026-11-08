#ifndef STORY_MODELS_H
#define STORY_MODELS_H

#include <QList>
#include <QObject>
#include <QString>


class Profile;
class QNetworkAccessManager;
class QNetworkReply;

/** One model as its provider describes it. */
struct StoryModel
{
	QString id;
	QString name;
	bool images = false;
	bool free = false;
	int context = 0;
};

/**
 * Reads the provider's model list (`/models`, the OpenAI-compatible endpoint OpenRouter and LM Studio both serve)
 * to find the models that take an image and answer with text. A provider that cannot be reached, or that does not
 * describe its models, simply yields no suggestions; nothing here ever picks a paid model on its own.
 */
class StoryModelCatalog : public QObject
{
	Q_OBJECT

	public:
		explicit StoryModelCatalog(Profile *profile, QObject *parent = nullptr, QNetworkAccessManager *manager = nullptr);

		void fetch();
		/** Parses an OpenAI-compatible model list; separate so the rules can be tested without a network. */
		static QList<StoryModel> parse(const QByteArray &json);

	signals:
		void ready(const QList<StoryModel> &models);
		void failed(const QString &error);

	private:
		Profile *m_profile;
		QNetworkAccessManager *m_manager;
		QNetworkReply *m_reply = nullptr;
};

#endif // STORY_MODELS_H
