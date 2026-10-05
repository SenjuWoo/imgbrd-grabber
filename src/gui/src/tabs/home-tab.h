#ifndef HOME_TAB_H
#define HOME_TAB_H

#include <QWidget>
#include "models/library-recommendations.h"

class ImageLibraryActions;
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
		void reload();

	signals:
		void libraryRequested(qint64 collection, int smartFilter);
		void pictureRequested(const QString &key, const QStringList &keys, qint64 collection);

	protected:
		void showEvent(QShowEvent *event) override;

	private:
		void scheduleReload();
		void updateSelection();
		void updateModelStatus();
		void openSelected();
		QString selectedKey() const;
		Profile *m_profile;
		LibraryStore *m_store;
		LibraryRecommendations *m_recommendations;
		QComboBox *m_scope;
		QComboBox *m_mode;
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
		bool m_reloadPending = false;
};

#endif // HOME_TAB_H
