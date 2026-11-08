#ifndef PICTURE_STORY_H
#define PICTURE_STORY_H

#include <QObject>
#include <QPointer>
#include <QSharedPointer>
#include <QString>
#include <QStringList>
#include "ai/story-models.h"


class Image;
class Profile;
class QNetworkAccessManager;
class QNetworkReply;
class QUrl;
class StoryModelCatalog;

/**
 * Writes the story behind a picture with an OpenAI-compatible chat API (OpenRouter by default).
 *
 * Settings: `Story/baseUrl`, `Story/model`, `Story/writerModel` and `Story/quality` in the profile settings, the API
 * key in its own `story-key.dat` file so it is never written into a profile backup. Keyless localhost servers
 * (LM Studio, llama.cpp, Ollama's OpenAI endpoint) work without a key.
 *
 * The picture itself is sent whenever a copy can be read or a URL exists, so the answer rests on what is visible.
 * A tags-only story only happens when the caller asks for one. Quality mode reads the scene first and writes from
 * that description instead of asking for prose in a single shot.
 */
class PictureStory : public QObject
{
	Q_OBJECT

	public:
		/** Why a request stopped, so the caller can offer the right recovery instead of one generic error. */
		enum class Error {
			None,
			Config,
			NoPicture,
			InvalidModel,
			ImageRejected,
			RateLimited,
			Provider,
			Cancelled,
		};

		explicit PictureStory(Profile *profile, QObject *parent = nullptr, QNetworkAccessManager *manager = nullptr);
		~PictureStory() override;

		/** Ready to send: a valid base URL, and either an API key or a local (keyless) server. */
		static bool configured(Profile *profile);
		static QString baseUrl(Profile *profile);
		static QString apiKey(Profile *profile);
		/** Writes the key to its own file; false when it could not be written. */
		static bool setApiKey(Profile *profile, const QString &key);
		static QString model(Profile *profile);
		static void configure(Profile *profile, const QString &baseUrl, const QString &apiKey, const QString &model);
		/** `baseUrl` normalised to the API root: no trailing slash, and a pasted chat endpoint stripped. */
		static QUrl baseEndpoint(const QString &baseUrl);
		/** The chat completions URL; empty when the base URL is unusable. */
		static QUrl endpoint(const QString &baseUrl);
		/** The model list URL, the same root the chat endpoint lives under. */
		static QUrl modelsEndpoint(const QString &baseUrl);
		/** Whether the endpoint is this machine, where a missing key and plain HTTP are fine. */
		static bool isLocal(const QUrl &url);
		/** Why the current settings cannot be used, empty when they can. */
		static QString settingsProblem(Profile *profile);

		static bool quality(Profile *profile);
		static void setQuality(Profile *profile, bool quality);
		/** Model used for the writing pass in quality mode; empty means "the same model". */
		static QString writerModel(Profile *profile);
		static void setWriterModel(Profile *profile, const QString &model);
		/** True when the vision model is resolved from the provider's list instead of the settings field. */
		static bool autoModel(Profile *profile);
		static void setAutoModel(Profile *profile, bool automatic);
		/** The free vision model the last automatic lookup chose; empty when there is none yet. */
		static QString cachedAutoModel(Profile *profile);
		static void setCachedAutoModel(Profile *profile, const QString &model);
		/** The free text model automatic mode uses for tags-only stories. */
		static QString cachedAutoTextModel(Profile *profile);
		static void setCachedAutoTextModel(Profile *profile, const QString &model);
		/** A model id the user wants automatic selection to use when it is eligible. */
		static QString preferredModel(Profile *profile);
		static void setPreferredModel(Profile *profile, const QString &model);
		/**
		 * Picks from the provider's list: the preferred id when eligible, else a named heuristic (models that
		 * advertise vision in their own id, then the larger context window). Provider metadata carries no quality
		 * score, so this is a stable guess, not a ranking.
		 */
		static QString chooseModel(const QList<StoryModel> &models, bool needVision, const QString &preferred = {});

		/**
		 * Asks for one picture's story; emits ready() or failed() once.
		 * `localPath` is used for imported pictures that have no public URL.
		 * `allowTagsOnly` is the caller's explicit "write from tags alone" choice: without it a picture that cannot be
		 * sent is an error, never a quiet tags-only story.
		 */
		void generate(const QSharedPointer<Image> &image, const QString &localPath = {}, bool allowTagsOnly = false);
		void abort();
		bool busy() const;
		/** Whether the story rests on the picture itself (sent directly, or read by the vision pass). */
		bool usedPicture() const;
		/** True when the caller asked for a tags-only story after the picture could not be used. */
		bool visionFallback() const;
		Error errorKind() const;
		/** The model the last request actually went to (the resolved one when it was chosen automatically). */
		QString modelUsed() const;
		/** The model that read the picture in quality mode; empty when no scene pass ran. */
		QString visionModel() const;
		/** The picture encoded for a vision request, downscaled and bounded; empty when it cannot be read. */
		static QString imageDataUrl(const QString &path);
		/** Story-relevant tags first, budgeted, so actions and clothing are not lost behind a long general list. */
		static QStringList prioritizedTags(const QStringList &tags, int budget = 60);

	signals:
		void ready(const QString &key, const QString &text);
		void failed(const QString &key, const QString &error);

	private:
		void resolveAutoModel(bool needVision);
		void startRequest();
		void send(bool withPicture);
		void finished();
		void fail(Error kind, const QString &message);
		QString metadataBlock(const Image &image) const;

		Profile *m_profile;
		QNetworkAccessManager *m_manager;
		QNetworkReply *m_reply = nullptr;
		QPointer<StoryModelCatalog> m_catalog;
		// Bumped on every abort(): a reply or catalogue answer from an older run must never touch this one.
		int m_generation = 0;
		QSharedPointer<Image> m_image;
		QString m_key;
		QString m_prompt;
		QString m_scene;
		QString m_pictureUrl;
		QString m_localDataUrl;
		QString m_model;
		QString m_visionModel;
		QString m_writerModel;
		bool m_allowTagsOnly = false;
		bool m_quality = false;
		bool m_sceneStage = false;
		bool m_pictureAttached = false;
		// Free reasoning models can spend a small budget entirely on thinking; one retry with more room fixes it.
		int m_maxTokens = 2000;
		bool m_budgetRetried = false;
		bool m_usedPicture = false;
		bool m_visionFallback = false;
		Error m_error = Error::None;
};

#endif // PICTURE_STORY_H
