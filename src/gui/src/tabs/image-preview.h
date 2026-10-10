#ifndef IMAGE_PREVIEW_H
#define IMAGE_PREVIEW_H

#include <functional>
#include <QList>
#include <QObject>
#include <QPixmap>
#include <QPointer>
#include <QSet>
#include <QSharedPointer>
#include <QString>
#include <QUrl>


class DownloadQueue;
class Image;
class ImageLibraryActions;
class MainWindow;
class ThumbnailLoader;
class Profile;
class QBouton;
class QMenu;
class QMovie;
class QWidget;

class ImagePreview : public QObject
{
	Q_OBJECT

	public:
		ImagePreview(QSharedPointer<Image> image, QWidget *container, Profile *profile, DownloadQueue *downloadQueue, MainWindow *mainWindow, QObject *parent = nullptr);
		~ImagePreview() override;
		QWidget *container() const { return m_container; }
		void setCustomContextMenu(std::function<void (QMenu *, const QSharedPointer<Image> &)> customContextMenu);

	public slots:
		void load();
		void abort();
		void setChecked(bool checked);
		void setDownloadProgress(qint64 v1, qint64 v2);
		void refreshDensity();

	protected:
		void showLoadingMessage();
		void finishedLoading();
		bool eventFilter(QObject *object, QEvent *event) override;

	protected slots:
		void customContextMenuRequested();
		void contextSaveImage();
		void contextSaveImageAs();
		void contextSaveImageProgress(const QSharedPointer<Image> &img, qint64 v1, qint64 v2);
		void toggledWithId(int id, bool toggle, bool range);

	signals:
		void finished();
		void clicked();
		void toggled(bool toggle, bool range);

	private:
		QSharedPointer<Image> m_image;
		QPointer<QWidget> m_container;
		Profile *m_profile;
		DownloadQueue *m_downloadQueue;
		MainWindow *m_mainWindow;
		static QMovie *m_loadingMovie;

		ThumbnailLoader *m_loader;
		bool m_aborted = false;
		bool m_checked = false;
		QString m_previewError;
		QPixmap m_displayImage;
		int m_borderSize = 0;
		QString m_counter;
		QPointer<QBouton> m_bouton = nullptr;
		QPointer<ImageLibraryActions> m_actions = nullptr;
		void updateActionsVisibility();
		std::function<void (QMenu *, const QSharedPointer<Image> &)> m_customContextMenu = nullptr;
};

#endif // IMAGE_PREVIEW_H
