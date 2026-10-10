#ifndef PICTURE_ACTIONS_H
#define PICTURE_ACTIONS_H

#include <QList>
#include <QSharedPointer>
#include <QString>


class DownloadQueue;
class Image;
class LibraryStore;
class Profile;
class QWidget;

/** Actions shared by Discover and Library so both behave the same. */
namespace PictureActions
{
	/** Queues original-file downloads to the usual folder and confirms with a toast on `page`. Returns the queued count. */
	int download(Profile *profile, DownloadQueue *queue, const QList<QSharedPointer<Image>> &images, QWidget *page);

	/**
	 * Toggles Like or Favorite (they are exclusive) in `scope`, adding the picture to that collection when needed.
	 * Returns whether the rating is now on; `ok` reports storage errors.
	 */
	bool toggleRating(LibraryStore *store, const QString &key, qint64 scope, bool favorite, bool *ok = nullptr);
}

#endif // PICTURE_ACTIONS_H
