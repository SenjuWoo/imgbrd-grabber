#ifndef HOME_TAB_H
#define HOME_TAB_H

#include <QWidget>
#include <QPointer>
#include <QSharedPointer>
#include "models/library-recommendations.h"

class Image;
class ImageLibraryActions;
class ImagePreview;
class Page;
class QHideEvent;
class QTimer;
class MainWindow;
class Profile;
class QComboBox;
class QLabel;
class QListWidget;
class QProgressBar;
class QPushButton;
class QShowEvent;

class HomeTab : public QWidget
{
	Q_OBJECT

	public:
		HomeTab(Profile *profile, MainWindow *parent);
		~HomeTab() override;
		void reload();

	signals:
		void libraryRequested(qint64 collection, int smartFilter);
		void pictureRequested(const QString &key, const QStringList &keys, qint64 collection);

	protected:
		void showEvent(QShowEvent *event) override;
		void hideEvent(QHideEvent *event) override;

	private:
		void scheduleReload();
		void cancelDiscovery();
		void startDiscovery(const QString &session);
		void finishDiscovery(Page *page, bool success);
		void showDiscovery(const QString &selected);
		QSharedPointer<Image> discoveredImage(const QString &key) const;
		void updateSelection();
		void updateDensity();
		void updateModelStatus();
		void openSelected();
		QString selectedKey() const;
		MainWindow *m_mainWindow;
		Profile *m_profile;
		LibraryStore *m_store;
		LibraryRecommendations *m_recommendations;
		QComboBox *m_scope;
		QComboBox *m_mode;
		QComboBox *m_density;
		QComboBox *m_pictureCount;
		QPushButton *m_refresh;
		QLabel *m_hint;
		QLabel *m_coverage;
		QLabel *m_status;
		QLabel *m_empty;
		QLabel *m_selectionHint;
		QListWidget *m_grid;
		QProgressBar *m_progress;
		QPushButton *m_setup;
		QPushButton *m_update;
		QPushButton *m_cancel;
		QPushButton *m_add;
		QPushButton *m_view;
		QPushButton *m_hide;
		QPushButton *m_restore;
		ImageLibraryActions *m_actions;
		qint64 m_collection = 0;
		QHash<qint64, quint64> m_rotations;
		QHash<Page*, LibraryDiscoveryTopic> m_discoveryPages;
		QList<QPointer<ImagePreview>> m_previewLoaders;
		QList<QPointer<QWidget>> m_previewContainers;
		QList<QSharedPointer<Image>> m_discoveryImages;
		QHash<QString, QString> m_discoveryReasons;
		QStringList m_discoveryErrors;
		QString m_discoverySession;
		QTimer *m_discoveryTimeout;
		quint64 m_discoveryGeneration = 0;
		bool m_discoveryHadTopics = false;
		bool m_reloadPending = false;
};

#endif // HOME_TAB_H
