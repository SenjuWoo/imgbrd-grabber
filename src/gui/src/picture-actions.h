#ifndef PICTURE_ACTIONS_H
#define PICTURE_ACTIONS_H

#include <functional>
#include <QList>
#include <QStringList>
#include <QSharedPointer>
#include <QString>


class DiscoveryFeed;
class DownloadQueue;
class Image;
class LibraryStore;
class Profile;
class QMenu;
class QWidget;

/** Actions shared by Discover, Search and Library so they behave the same. */
namespace PictureActions
{
	/** Queues original-file downloads to the usual folder and confirms with a toast on `page`. Returns the queued count. */
	int download(Profile *profile, DownloadQueue *queue, const QList<QSharedPointer<Image>> &images, QWidget *page);

	/**
	 * Toggles Like or Favorite (they are exclusive) in `scope`, adding the picture to that collection when needed.
	 * Returns whether the rating is now on; `ok` reports storage errors.
	 */
	bool toggleRating(LibraryStore *store, const QString &key, qint64 scope, bool favorite, bool *ok = nullptr);

	/**
	 * After un-rating, offers to remove the pictures that are no longer liked, favorited, collected or annotated.
	 * Asks without blocking unless "Library/removeUnrated" remembers an answer (1 remove, 2 keep).
	 */
	void offerRemoval(Profile *profile, const QStringList &keys, QWidget *parent);

	/**
	 * Fills a "Not interested" menu for `count` pictures like `image`: hide them, see less of one of its tags, never see a tag,
	 * or name another tag. `act(tags, never)` carries out the choice; no tags means hide only.
	 */
	void fillNotInterested(QMenu *menu, DiscoveryFeed *feed, const QSharedPointer<Image> &image, int count, const std::function<void(const QStringList &, bool)> &act, QWidget *parent);

	/** What hiding did, for the toast offering Undo. */
	QString notInterestedMessage(const QStringList &tags, bool never);
}

#endif // PICTURE_ACTIONS_H
