#ifndef LIBRARY_TAB_H
#define LIBRARY_TAB_H

#include <QHash>
#include <QSharedPointer>
#include <QWidget>
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

class LibraryTab : public QWidget
{
	Q_OBJECT

	public:
		LibraryTab(Profile *profile, MainWindow *parent);
		void reload();

	private:
		void scheduleReload();
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
};

#endif // LIBRARY_TAB_H
