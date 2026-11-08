#ifndef PICTURE_STORY_H
#define PICTURE_STORY_H

#include <QObject>
#include <QSharedPointer>
#include <QString>


class Image;
class Profile;
class QNetworkAccessManager;
class QNetworkReply;
class QUrl;

/**
 * Writes the story behind a picture with an OpenAI-compatible chat API (OpenRouter by default).
 *
 * Settings: `Story/baseUrl` and `Story/model` in the profile settings, the API key in its own
 * `story-key.dat` file so it is never written into a profile backup. Keyless localhost servers
 * (LM Studio, llama.cpp, Ollama's OpenAI endpoint) work without a key.
 *
 * The picture's tags, title and source are always sent; the picture itself is sent when it can be
 * reached (remote URL) or read (local import), downscaled first. A provider that refuses the image
 * is retried once from the tags alone.
 */
class PictureStory : public QObject
{
	Q_OBJECT

	public:
		explicit PictureStory(Profile *profile, QObject *parent = nullptr);
		~PictureStory() override;

		/** Ready to send: a valid base URL, and either an API key or a local (keyless) server. */
		static bool configured(Profile *profile);
		static QString baseUrl(Profile *profile);
		static QString apiKey(Profile *profile);
		static void setApiKey(Profile *profile, const QString &key);
		static QString model(Profile *profile);
		static void configure(Profile *profile, const QString &baseUrl, const QString &apiKey, const QString &model);
		/** `baseUrl` with the chat endpoint appended and trailing slashes removed; empty when unusable. */
		static QUrl endpoint(const QString &baseUrl);
		/** Whether the endpoint is this machine, where a missing key and plain HTTP are fine. */
		static bool isLocal(const QUrl &url);
		/** Why the current settings cannot be used, empty when they can. */
		static QString settingsProblem(Profile *profile);

		/**
		 * Asks for one picture's story; emits ready() or failed() once.
		 * `localPath` is used for imported pictures that have no public URL.
		 */
		void generate(const QSharedPointer<Image> &image, const QString &localPath = {});
		void abort();
		bool busy() const;

	signals:
		void ready(const QString &key, const QString &text);
		void failed(const QString &key, const QString &error);

	private:
		void send(bool withPicture);
		void finished();
		static QString imageDataUrl(const QString &path);
		Profile *m_profile;
		QNetworkAccessManager *m_manager;
		QNetworkReply *m_reply = nullptr;
		QSharedPointer<Image> m_image;
		QString m_key;
		QString m_prompt;
		QString m_pictureUrl;
		QString m_localDataUrl;
		QString m_model;
		QString m_baseUrl;
		bool m_retried = false;
};

#endif // PICTURE_STORY_H
