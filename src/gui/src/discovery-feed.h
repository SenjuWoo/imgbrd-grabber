#ifndef DISCOVERY_FEED_H
#define DISCOVERY_FEED_H

#include <QDateTime>
#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QSharedPointer>
#include <QStringList>
#include <QVector>
#include "models/taste-profile.h"
#include "utils/image-fingerprint.h"


class Image;
class LibraryRecommendations;
class Page;
class Profile;
class QTimer;
class ThumbnailLoader;
class VisualEncoder;

struct DiscoveryItem
{
	QString key;
	QSharedPointer<Image> image;
	QString reason;
	double score = 0;
	bool visual = false;
	QStringList matches;
};

/** A picture hidden with "Not interested", kept so it can be restored. */
struct HiddenPicture
{
	QString key;
	QSharedPointer<Image> image;
	QDateTime hiddenAt;
};

/**
 * Endless, personal picture feed. Each batch searches the selected sources with real tags learned
 * from likes and favorites, drops pictures already rated, hidden, blacklisted, seen or duplicated
 * across sources, optionally ranks previews with the local visual model, and emits the best ones.
 */
class DiscoveryFeed : public QObject
{
	Q_OBJECT

	public:
		DiscoveryFeed(Profile *profile, LibraryRecommendations *recommendations, QObject *parent = nullptr);
		~DiscoveryFeed() override;

		void setScope(qint64 collection);
		qint64 scope() const;
		void setVisualEnabled(bool enabled);
		bool visualActive() const;
		bool visualEnabled() const { return m_visualEnabled; }
		void restart();
		void fetchMore();
		void cancel();
		/** While paused (Discover not on screen) no new batch starts and the running one stops, freeing source rate limits. */
		void setPaused(bool paused);
		bool isBusy() const;
		bool lastBatchProductive() const { return m_batchEmitted > 0; }
		const TasteProfile &taste();
		const TagBackground &background() const { return m_background; }
		const QHash<QString, DiscoveryFeedback> &feedback() const { return m_feedback; }
		/** How many hidden pictures carried each tag. */
		const QHash<QString, double> &hiddenTags() const { return m_dislikedTags; }
		QSharedPointer<Image> image(const QString &key) const;
		/**
		 * Hides a picture. Without tags, all its tags count slightly against it; with tags, only those are
		 * turned into "show less" (or "never show") preferences.
		 */
		void dismiss(const QString &key, const QStringList &tags = {}, bool never = false, const QSharedPointer<Image> &image = {});
		/** Whether a picture was hidden with "Not interested", here or in a search. */
		bool isHidden(const QString &key) const;
		/** True while a picture shown less than `tuning().seenHours` ago (or ever, with 0 hours) should stay out of the feed. */
		bool isSeen(const QString &key) const;
		/** The first of these tags the user never wants to see, or an empty string. */
		QString neverShown(const QStringList &tags) const;
		void resetDismissed();
		int dismissedCount() const;
		/** Hidden pictures that can be shown again, newest first. */
		QList<HiddenPicture> hiddenPictures() const;
		/** Pictures hidden before 7.19, which kept only their key: no preview or tags to show. */
		QStringList legacyHidden() const;
		/** Shows hidden pictures again and takes back the penalties hiding them added. */
		void restore(const QStringList &keys);
		const TasteTuning &tuning() const;
		void setTuning(const TasteTuning &tuning);
		static QString statePath(const QString &profileDirectory);

	signals:
		void itemsReady(const QList<DiscoveryItem> &items);
		void busyChanged(bool busy);
		void statusChanged(const QString &status);

	private:
		struct HiddenRecord
		{
			QJsonObject image;
			QStringList penalized;
			QString at;
		};
		struct Candidate
		{
			DiscoveryItem item;
			QStringList query;
			double tagScore = 0;
			double visualScore = -1;
			ImageFingerprint fingerprint;
			bool done = false;
			bool dropped = false; // A better tagged copy of the same picture replaced it.
			int group = 0;
		};

		void loadState();
		void saveState() const;
		void pruneSeen();
		void rebuildTaste();
		void pageFinished(Page *page, bool success);
		void emitGroup(int group, bool force);
		void finishIfIdle();
		void startNextPreview();
		void previewFinished(ThumbnailLoader *loader, const QString &key);
		void encoded(const QString &key, const QVector<float> &vector);
		void candidateDone(const QString &key);
		void ratingChanged();
		void finishBatch();
		void setBusy(bool busy);
		double visualScore(const QVector<float> &vector) const;

		Profile *m_profile;
		LibraryRecommendations *m_recommendations;
		qint64 m_scope = 0;
		bool m_visualEnabled = false;
		VisualEncoder *m_encoder = nullptr;
		TasteProfile m_taste;
		bool m_tasteDirty = true;
		quint32 m_seed = 0;
		int m_batch = 0;
		int m_emptyBatches = 0;
		bool m_busy = false;
		QTimer *m_timeout;

		QHash<Page *, QString> m_pages;
		QHash<QString, QString> m_queryReasons;
		QHash<QString, int> m_nextPage;
		QSet<QString> m_exhausted;
		QStringList m_failedSources;

		QHash<QString, Candidate> m_candidates;
		QHash<int, QStringList> m_groups;
		int m_nextGroup = 0;
		int m_batchEmitted = 0;
		int m_batchVisual = 0;
		bool m_paused = false;
		QList<QString> m_previewQueue;
		QList<QPointer<ThumbnailLoader>> m_loaders;

		QHash<QString, QSharedPointer<Image>> m_shown;
		QList<ImageFingerprint> m_shownFingerprints;
		QSet<QString> m_shownMd5;
		// Pictures already shown: key (and "md5:<md5>" so another source's copy counts too) -> when, in epoch seconds.
		QHash<QString, qint64> m_seen;
		QStringList m_hidden;
		QSet<QString> m_hiddenSet;
		QHash<QString, HiddenRecord> m_hiddenRecords;
		TasteTuning m_tuning;
		QHash<QString, double> m_dislikedTags;
		TagBackground m_background;
		bool m_backgroundChanged = false;
		QHash<QString, DiscoveryFeedback> m_feedback;
		QHash<QString, QStringList> m_shownQueries;
		QSet<QString> m_credited;
		QSet<QString> m_unsampleable;
		QList<QPair<QVector<float>, double>> m_visualSeeds;
};

#endif // DISCOVERY_FEED_H
