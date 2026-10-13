#ifndef LIBRARY_HEALTH_DIALOG_H
#define LIBRARY_HEALTH_DIALOG_H

#include <QDialog>
#include <QHash>
#include "models/library-health.h"

class LibraryStore;
class Profile;
class QLabel;
class QListWidget;
class QPushButton;

/** Shows what is wrong with the Library (missing files, no tags, duplicates...) and lets you open or remove the affected pictures. */
class LibraryHealthDialog : public QDialog
{
	Q_OBJECT

	public:
		LibraryHealthDialog(Profile *profile, QWidget *parent = nullptr);
		const LibraryHealth &health() const { return m_health; }

	signals:
		void pictureRequested(const QString &key, const QStringList &keys);
		void libraryChanged();

	private:
		void refresh();
		void showIssue(int row);
		void removeSelected();
		Profile *m_profile;
		LibraryStore *m_store;
		LibraryHealth m_health;
		QHash<QString, LibraryEntry> m_entries;
		QLabel *m_summary;
		QLabel *m_hint;
		QListWidget *m_issues;
		QListWidget *m_pictures;
		QPushButton *m_open;
		QPushButton *m_remove;
};

#endif // LIBRARY_HEALTH_DIALOG_H
