#ifndef LIBRARY_STORE_H
#define LIBRARY_STORE_H

#include <QByteArray>
#include <QJsonObject>
#include <QObject>
#include <QSet>
#include <QSqlDatabase>
#include <QVariant>
#include <QStringList>

class Image;
struct LibraryImportData;

struct LibraryEntry
{
	QString key;
	QJsonObject image;
	QByteArray thumbnail;
	bool liked = false;
	bool favorite = false;
	QString notes;
	QString savedAt;
	int collectionCount = 0;
	QStringList localPaths;
	QStringList tags() const;
	QStringList metadataErrors() const;
};

struct LibraryCollection
{
	qint64 id;
	QString name;
	int count;
	QByteArray cover;
};

class LibraryStore : public QObject
{
	Q_OBJECT

	public:
		explicit LibraryStore(const QString &path, QObject *parent = nullptr);
		~LibraryStore() override;
		bool isReady() const;
		QString lastError() const;
		static QString imageKey(const Image &image);
		QString keyForImage(const Image &image);
		QString saveImage(const Image &image);
		QString saveLocalImage(const LibraryImportData &data, qint64 collection = 0, const QString &expectedKey = {});
		bool linkSource(const QString &key, const Image &image, const QString &evidence);
		bool backupTo(const QString &path);
		bool restoreFrom(const QString &path);
		QList<LibraryEntry> entries(qint64 collection = 0);
		LibraryEntry entry(const QString &key, qint64 collection = 0);
		bool setLiked(const QString &key, bool liked, qint64 collection = 0);
		bool setFavorite(const QString &key, bool favorite, qint64 collection = 0);
		bool setNotes(const QString &key, const QString &notes, qint64 collection = 0);
		bool removeImage(const QString &key);
		QList<LibraryCollection> collections();
		qint64 createCollection(const QString &name);
		bool renameCollection(qint64 id, const QString &name);
		bool removeCollection(qint64 id);
		bool setCollectionCover(qint64 id, const QString &key);
		bool addToCollection(const QString &key, qint64 collection);
		bool removeFromCollection(const QString &key, qint64 collection);
		bool contains(const QString &key, qint64 collection = 0);
		/** Whether this picture, or the same file from another source, is liked or favorited in any scope. */
		bool isRated(const Image &image);

	signals:
		void imageChanged(const QString &key);
		void collectionsChanged();

	private:
		bool execute(const QString &sql, const QVariantList &values = {});
		bool setValue(const QString &key, const QString &column, const QVariant &value, qint64 collection);
		QString storedPath(const QString &path) const;
		QString resolvedPath(const QString &path) const;
		void loadRatedIndex();
		QSet<QString> m_ratedKeys;
		QSet<QString> m_ratedMd5s;
		bool m_ratedDirty = true;
		QString m_directory;
		QString m_connection;
		QSqlDatabase m_database;
		QString m_error;
		bool m_ready = false;
};

#endif // LIBRARY_STORE_H
