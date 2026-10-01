#ifndef LIBRARY_IMAGE_DIALOG_H
#define LIBRARY_IMAGE_DIALOG_H

#include <QDialog>
#include <QPointer>
#include <QStringList>
#include "models/library-store.h"

class ImageLibraryActions;
class LibraryStore;
class Profile;
class QGraphicsView;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QUrl;

class LibraryImageDialog : public QDialog
{
	Q_OBJECT

	public:
		LibraryImageDialog(Profile *profile, const QStringList &keys, const QString &key, qint64 collection, QWidget *parent = nullptr);
		void reject() override;

	signals:
		void locateRequested(const QString &key);
		void sourceRequested(const QString &key);

	protected:
		bool eventFilter(QObject *watched, QEvent *event) override;
		void resizeEvent(QResizeEvent *event) override;
		void showEvent(QShowEvent *event) override;
		void closeEvent(QCloseEvent *event) override;

	private:
		void showCurrent();
		void navigate(int direction);
		void fitImage();
		void zoomImage(qreal factor);
		bool saveNotes();
		Profile *m_profile;
		LibraryEntry m_lastEntry;
		QPointer<LibraryStore> m_store;
		QStringList m_keys;
		QString m_filePath;
		QString m_sourceUrl;
		int m_index = 0;
		qint64 m_collection;
		bool m_fit = true;
		QGraphicsView *m_view = nullptr;
		ImageLibraryActions *m_actions;
		QLabel *m_title;
		QLabel *m_position;
		QLabel *m_status;
		QPlainTextEdit *m_metadata;
		QPlainTextEdit *m_rawMetadata;
		QPlainTextEdit *m_notes;
		QPushButton *m_previous;
		QPushButton *m_next;
		QPushButton *m_reveal;
		QPushButton *m_open;
		QPushButton *m_source;
		QPushButton *m_saveNotes;
};

#endif // LIBRARY_IMAGE_DIALOG_H
