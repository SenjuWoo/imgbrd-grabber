#ifndef PICTURE_STORY_H
#define PICTURE_STORY_H

#include <QObject>
#include <QSharedPointer>
#include <QString>


class Image;
class Profile;
class QNetworkAccessManager;
class QNetworkReply;

/**
 * Writes the story behind a picture with an OpenAI-compatible chat API (OpenRouter by default).
 *
 * Settings, kept in the profile so they survive restarts: `Story/baseUrl` (default
 * https://openrouter.ai/api/v1), `Story/apiKey` and `Story/model` (default openrouter/auto).
 * The picture's tags, title, source and, when it has a public URL, the picture itself are sent;
 * a provider that refuses remote images is retried once from the tags alone.
 */
class PictureStory : public QObject
{
	Q_OBJECT

	public:
		explicit PictureStory(Profile *profile, QObject *parent = nullptr);
		~PictureStory() override;

		/** Ready to send: a base URL and an API key are set. */
		static bool configured(Profile *profile);
		static QString baseUrl(Profile *profile);
		static QString apiKey(Profile *profile);
		static QString model(Profile *profile);
		static void configure(Profile *profile, const QString &baseUrl, const QString &apiKey, const QString &model);

		/** Asks for one picture's story; emits ready() or failed() once. */
		void generate(const QSharedPointer<Image> &image);
		void abort();
		bool busy() const;

	signals:
		void ready(const QString &key, const QString &text);
		void failed(const QString &key, const QString &error);

	private:
		void send(bool withPicture);
		void finished();
		Profile *m_profile;
		QNetworkAccessManager *m_manager;
		QNetworkReply *m_reply = nullptr;
		QSharedPointer<Image> m_image;
		QString m_key;
		QString m_prompt;
		QString m_pictureUrl;
		QString m_model;
		bool m_retried = false;
};

#endif // PICTURE_STORY_H
