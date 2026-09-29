#ifndef IMAGE_LIBRARY_ACTIONS_H
#define IMAGE_LIBRARY_ACTIONS_H

#include <QSharedPointer>
#include <QWidget>

class QAction;
class Image;
class QLabel;
class LibraryStore;
class Profile;
class QMenu;
class QToolButton;

// One action implementation for search cards, context menus, the viewer and Library.
class ImageLibraryActions : public QWidget
{
	Q_OBJECT

	public:
		ImageLibraryActions(Profile *profile, const QSharedPointer<Image> &image, QWidget *parent = nullptr, qint64 collection = 0, bool compact = false);
		void setImage(const QSharedPointer<Image> &image, qint64 collection = 0);
		void setSelection(const QList<QSharedPointer<Image>> &images, const QStringList &keys, qint64 collection = 0);
		void addToMenu(QMenu *menu);
		void refresh();

	private:
		bool saveSelection();
		void showError();
		void populateCollections(QMenu *menu);
		Profile *m_profile;
		LibraryStore *m_store;
		QList<QSharedPointer<Image>> m_images;
		QStringList m_keys;
		QList<QMetaObject::Connection> m_imageConnections;
		qint64 m_collection = 0;
		bool m_compact;
		QAction *m_like;
		QAction *m_favorite;
		QAction *m_save;
		QToolButton *m_likeButton;
		QToolButton *m_favoriteButton;
		QToolButton *m_collectButton;
		QLabel *m_scopeLabel;
};

#endif // IMAGE_LIBRARY_ACTIONS_H
