#ifndef LIBRARY_SOURCE_DIALOG_H
#define LIBRARY_SOURCE_DIALOG_H

#include <QDialog>
#include <QJsonObject>
#include <QList>
#include <QPointer>
#include <QSharedPointer>
#include <atomic>
#include <memory>
#include "models/pixiv-artwork.h"

class Image;
class LibraryStore;
class NetworkReply;
class Page;
class Profile;
class QComboBox;
class QLabel;
class QListWidget;
class QPushButton;
class QTimer;
class QUrl;

class LibrarySourceDialog : public QDialog
{
	Q_OBJECT

	public:
		LibrarySourceDialog(Profile *profile, const QString &key, QWidget *parent = nullptr);
		~LibrarySourceDialog() override;
		static bool isSourceUrl(const QUrl &url);

	private:
		struct Candidate
		{
			QSharedPointer<Image> image;
			QString evidence;
		};
		void exactLookup();
		void cancelLookup();
		void showExactResults(Page *page, const QString &hash);
		void lookupNext(const QString &hash);
		void findSimilar();
		void clearCandidates();
		void addCandidate(const QSharedPointer<Image> &image, const QByteArray &thumbnail, const QString &label, const QString &evidence);
		void linkSelected();
		void findOnPixiv();
		void pixivDetailsLoaded(NetworkReply *reply);
		void tryPixivFile();
		void addPixivCandidate(const QUrl &file, const QString &md5, bool exact);
		Profile *m_profile;
		LibraryStore *m_store;
		QString m_key;
		QJsonObject m_local;
		QByteArray m_thumbnail;
		QComboBox *m_source;
		QComboBox *m_hash;
		QPushButton *m_lookup;
		QPushButton *m_similar;
		QPushButton *m_pixiv;
		QPair<QString, int> m_pixivId;
		PixivArtwork m_artwork;
		QList<QUrl> m_pixivFiles;
		QPointer<NetworkReply> m_pixivReply;
		QPushButton *m_link;
		QLabel *m_status;
		QListWidget *m_results;
		QTimer *m_timeout;
		QPointer<Page> m_page;
		QStringList m_lookupSites;
		int m_lookupTotal = 0;
		QList<Candidate> m_candidates;
		std::shared_ptr<std::atomic_bool> m_cancelVisual;
		int m_visualGeneration = 0;
};

#endif // LIBRARY_SOURCE_DIALOG_H
