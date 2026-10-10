#ifndef HOME_TAB_H
#define HOME_TAB_H

#include <QHash>
#include <QSharedPointer>
#include <QWidget>
#include "ui/image-grid.h"

class DiscoveryFeed;
class DownloadQueue;
class Image;
class LibraryRecommendations;
class LibraryStore;
class MainWindow;
class Profile;
class QComboBox;
class QFrame;
class QHBoxLayout;
class QLabel;
class QProgressBar;
class QPushButton;
class QStackedWidget;
class QToolButton;
struct DiscoveryItem;

/**
 * Discover: an endless feed of new pictures from the selected sources, learned from likes and
 * favorites (favorites count three times as much). Optional local AI ranks pictures that look alike.
 */
class HomeTab : public QWidget
{
	Q_OBJECT

	public:
		HomeTab(Profile *profile, MainWindow *parent, DownloadQueue *downloadQueue = nullptr);
		~HomeTab() override;
		void refresh();
		ImageGridView *grid() const;
		DiscoveryFeed *feed() const;

	signals:
		void libraryRequested(qint64 collection, int smartFilter);
		void searchRequested(const QString &tags);

	protected:
		void showEvent(QShowEvent *event) override;

	private:
		void addItems(const QList<DiscoveryItem> &items);
		void triggerAction(ImageGridView::Action action, const QStringList &keys);
		void rate(const QString &key, bool favorite);
		void openPicture(const QString &key);
		void showMenu(const QStringList &keys, const QPoint &position);
		void updateHeader();
		void updateChips();
		void updateAi();
		void updateScopes();
		void enableAi();
		void syncRating(const QString &libraryKey);
		QString libraryKey(const QString &key);

		Profile *m_profile;
		MainWindow *m_mainWindow;
		DownloadQueue *m_downloadQueue;
		LibraryStore *m_store;
		LibraryRecommendations *m_recommendations;
		DiscoveryFeed *m_feed;
		QLabel *m_subtitle;
		QComboBox *m_scope;
		QToolButton *m_more;
		QPushButton *m_refresh;
		QFrame *m_aiBanner;
		QLabel *m_aiText;
		QPushButton *m_aiEnable;
		QProgressBar *m_aiProgress;
		QWidget *m_chips;
		QHBoxLayout *m_chipsLayout;
		QProgressBar *m_busy;
		QStackedWidget *m_stack;
		ImageGridView *m_grid;
		QLabel *m_empty;
		QLabel *m_status;
		QHash<QString, QString> m_libraryKeys;
		bool m_started = false;
};

#endif // HOME_TAB_H
