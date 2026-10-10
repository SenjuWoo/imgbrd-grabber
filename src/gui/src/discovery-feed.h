#ifndef DISCOVERY_FEED_H
#define DISCOVERY_FEED_H

#include <QHash>
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
		void restart();
		void fetchMore();
		void cancel();
		bool isBusy() const;
		bool lastBatchProductive() const { return m_batchEmitted > 0; }
		const TasteProfile &taste();
		QSharedPointer<Image> image(const QString &key) const;
		void dismiss(const QString &key);
		void resetDismissed();
		int dismissedCount() const;
		static QString statePath(const QString &profileDirectory);

	signals:
		void itemsReady(const QList<DiscoveryItem> &items);
		void busyChanged(bool busy);
		void statusChanged(const QString &status);

	private:
		struct Candidate
		{
			DiscoveryItem item;
			double tagScore = 0;
			double visualScore = -1;
			ImageFingerprint fingerprint;
			bool done = false;
			int group = 0;
		};

		void loadState();
		void saveState() const;
		void rebuildTaste();
		void pageFinished(Page *page, bool success);
		void emitGroup(int group, bool force);
		void finishIfIdle();
		void startNextPreview();
		void previewFinished(ThumbnailLoader *loader, const QString &key);
		void encoded(const QString &key, const QVector<float> &vector);
		void candidateDone(const QString &key);
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
		QList<QString> m_previewQueue;
		QList<QPointer<ThumbnailLoader>> m_loaders;

		QHash<QString, QSharedPointer<Image>> m_shown;
		QList<ImageFingerprint> m_shownFingerprints;
		QSet<QString> m_shownMd5;
		QStringList m_seen;
		QSet<QString> m_seenSet;
		QStringList m_hidden;
		QSet<QString> m_hiddenSet;
		QHash<QString, double> m_dislikedTags;
		QList<QPair<QVector<float>, double>> m_visualSeeds;
};

#endif // DISCOVERY_FEED_H
