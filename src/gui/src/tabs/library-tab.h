#ifndef LIBRARY_TAB_H
#define LIBRARY_TAB_H

#include <QHash>
#include <QSharedPointer>
#include <QWidget>
#include <QSet>
#include <atomic>
#include <memory>
#include "models/library-store.h"

class Image;
class ImageLibraryActions;
class QLabel;
class QLineEdit;
class QListWidget;
class QComboBox;
class QTreeWidget;
class QPushButton;
class QStackedWidget;
class Profile;
class MainWindow;
class QProgressDialog;
class QTimer;
class QDragEnterEvent;
class QDropEvent;

class LibraryTab : public QWidget
{
	Q_OBJECT

	public:
		LibraryTab(Profile *profile, MainWindow *parent);
		~LibraryTab() override;
		void reload();
		void showView(qint64 collection = 0, int smartFilter = 0);
		void openPicture(const QString &key, const QStringList &keys, qint64 collection = 0);
		void importPaths(const QStringList &paths, bool copy = false, const QString &expectedKey = {});
		bool importing() const { return m_importing; }

	signals:
		void importFinished(int added, int duplicates, int failed);

	protected:
		void dragEnterEvent(QDragEnterEvent *event) override;
		void dropEvent(QDropEvent *event) override;

	private:
		void scheduleReload();
		void importNext();
		void finishImport();
		void locateFile(const QString &key);
		void findSource(const QString &key);
		void updateSelection();
		void collectionMenu(const QPoint &pos);
		void imageMenu(const QPoint &pos);
		void newCollection();
		void openImage(const QString &key);
		QSharedPointer<Image> restoreImage(const LibraryEntry &entry);
		Profile *m_profile;
		MainWindow *m_mainWindow;
		LibraryStore *m_store;
		QTreeWidget *m_sidebar;
		QPushButton *m_manage;
		QPushButton *m_more;
		QPushButton *m_findSource, *m_previousPage, *m_nextPage;
		QLabel *m_pageLabel;
		QStringList m_viewKeys;
		int m_page = 0;
		QListWidget *m_grid;
		QLineEdit *m_search;
		QComboBox *m_filter;
		QLabel *m_title;
		QLabel *m_count;
		QLabel *m_hint;
		QLabel *m_empty;
		QStackedWidget *m_stack;
		ImageLibraryActions *m_actions;
		QHash<QString, LibraryEntry> m_entries;
		qint64 m_collection = 0;
		int m_smartFilter = 0;
		bool m_reloadPending = false;
		QTimer *m_searchTimer;
		QProgressDialog *m_progress = nullptr;
		QPushButton *m_importButton;
		std::shared_ptr<std::atomic_bool> m_cancel;
		QStringList m_importFiles, m_importErrors;
		QSet<QString> m_knownKeys;
		QString m_managedDirectory, m_expectedKey;
		qint64 m_importCollection = 0;
		int m_importIndex = 0, m_added = 0, m_duplicates = 0, m_failed = 0;
		bool m_importing = false, m_copyImports = false;
};

#endif // LIBRARY_TAB_H
