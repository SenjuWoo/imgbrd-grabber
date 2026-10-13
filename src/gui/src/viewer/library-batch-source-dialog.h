#ifndef LIBRARY_BATCH_SOURCE_DIALOG_H
#define LIBRARY_BATCH_SOURCE_DIALOG_H

#include <QDialog>
#include <QList>
#include <QPointer>
#include <QSharedPointer>
#include <QStringList>

class Image;
class LibraryStore;
class Page;
class Profile;
class QLabel;
class QListWidget;
class QProgressBar;
class QPushButton;
class QTimer;

/** Looks up the exact MD5 of every imported picture that has no tags, so Discover can learn from them. Links only after the user checks them. */
class LibraryBatchSourceDialog : public QDialog
{
	Q_OBJECT

	public:
		LibraryBatchSourceDialog(Profile *profile, QWidget *parent = nullptr);
		~LibraryBatchSourceDialog() override;
		int pendingCount() const { return int(m_jobs.size()); }

	private:
		struct Job
		{
			QString key, name;
			QStringList hashes;
		};
		struct Match
		{
			QString key;
			QSharedPointer<Image> image;
			QString evidence;
		};
		void toggle();
		void stop();
		void nextJob();
		void nextStep();
		void showResults(Page *page, const QString &hash);
		void linkChecked();
		Profile *m_profile;
		LibraryStore *m_store;
		QList<Job> m_jobs;
		QList<Match> m_matches;
		QStringList m_sites;
		QList<QPair<QString, QString>> m_steps; // (hash, site url) still to ask for the current picture
		int m_job = -1;
		bool m_running = false;
		QPointer<Page> m_page;
		QTimer *m_timeout;
		QLabel *m_status;
		QProgressBar *m_progress;
		QListWidget *m_list;
		QPushButton *m_start;
		QPushButton *m_link;
};

#endif // LIBRARY_BATCH_SOURCE_DIALOG_H
