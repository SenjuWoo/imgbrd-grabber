#ifndef DISCOVER_DIALOGS_H
#define DISCOVER_DIALOGS_H

#include <QDialog>
#include <QHash>
#include <QList>
#include <QPointer>
#include <QSharedPointer>
#include "models/taste-profile.h"


class DiscoveryFeed;
class Image;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QTableWidget;
class ThumbnailLoader;

/** Shows what Discover learned and how it curates, and lets the user steer it: focus, variety and per-tag preferences. */
class DiscoverTasteDialog : public QDialog
{
	Q_OBJECT

	public:
		explicit DiscoverTasteDialog(DiscoveryFeed *feed, QWidget *parent = nullptr);
		/** The tuning as edited in the dialog. */
		TasteTuning tuning() const;

	private:
		void addRow(const TasteTag &tag, int preference);
		void addTag(int preference);
		void updateVariety();

		DiscoveryFeed *m_feed;
		QLineEdit *m_focus;
		QComboBox *m_variety;
		QLabel *m_varietyHint;
		QTableWidget *m_tags;
		QLineEdit *m_newTag;
};

/** Pictures hidden with "Not interested", so ones hidden by mistake can come back. */
class DiscoverHiddenDialog : public QDialog
{
	Q_OBJECT

	public:
		explicit DiscoverHiddenDialog(DiscoveryFeed *feed, QWidget *parent = nullptr);
		~DiscoverHiddenDialog() override;
		int restoredCount() const;

	private:
		void restoreKeys(const QStringList &keys);
		void loadNextThumbnails();
		void updateSummary();

		DiscoveryFeed *m_feed;
		QListWidget *m_list;
		QPushButton *m_restoreSelected;
		QPushButton *m_restoreAll;
		QLabel *m_summary;
		QStringList m_pending;
		QHash<QString, QSharedPointer<Image>> m_images;
		QList<QPointer<ThumbnailLoader>> m_loaders;
		int m_restored = 0;
};

#endif // DISCOVER_DIALOGS_H
