#ifndef LIBRARY_RECOMMENDATIONS_H
#define LIBRARY_RECOMMENDATIONS_H

#include <QObject>
#include <QDate>
#include <QHash>
#include <QVector>
#include <atomic>
#include <memory>
#include "models/library-recommender.h"

class Profile;
class QNetworkReply;
class QSaveFile;
class QTimer;

template <typename T> class QFutureWatcher;

struct LibraryIndexResult
{
	int total = 0, indexed = 0, reused = 0, skipped = 0, failed = 0;
	bool cancelled = false;
	QString error;
	QStringList errors;
	QHash<QString, QVector<float>> vectors;
	QHash<QString, QString> fingerprints;
};

// Derived local image index. Library metadata and personal ratings remain in LibraryStore.
class LibraryRecommendations : public QObject
{
	Q_OBJECT

	public:
		explicit LibraryRecommendations(Profile *profile, QObject *parent = nullptr);
		~LibraryRecommendations() override;
		bool busy() const;
		bool modelAvailable() const;
		int indexedCount() const;
		QString status() const;
		LibraryRecommendationResult recommendations(qint64 scope = 0, const QDate &day = QDate::currentDate(), int limit = 24);
		void startIndexing();
		void downloadModel();
		void cancel();
		void hide(const QString &key, qint64 scope);
		void restoreHidden(qint64 scope);
		static QString cachePath(const QString &profileDirectory);

	signals:
		void changed();
		void progress(int done, int total);
		void indexFinished(const LibraryIndexResult &result);

	private:
		void loadCache();
		void finishIndexing(bool notify);
		void invalidate(const QString &key);
		Profile *m_profile;
		QHash<QString, QVector<float>> m_vectors;
		QHash<QString, QString> m_fingerprints;
		QFutureWatcher<LibraryIndexResult> *m_watcher;
		std::shared_ptr<std::atomic_bool> m_cancel;
		QNetworkReply *m_reply = nullptr;
		QSaveFile *m_download = nullptr;
		QTimer *m_reindex;
		QString m_status;
		qint64 m_downloadBytes = 0;
		bool m_dirty = false;
		bool m_indexPending = false;
};

#endif // LIBRARY_RECOMMENDATIONS_H
