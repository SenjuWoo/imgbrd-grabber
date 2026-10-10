#include "picture-actions.h"
#include <QDir>
#include <QPointer>
#include <QSettings>
#include <QWidget>
#include <memory>
#include "downloader/download-queue.h"
#include "downloader/image-downloader.h"
#include "downloader/image-save-result.h"
#include "models/image.h"
#include "models/library-store.h"
#include "models/profile.h"
#include "ui/toast.h"


int PictureActions::download(Profile *profile, DownloadQueue *queue, const QList<QSharedPointer<Image>> &images, QWidget *page)
{
	QSettings *settings = profile->getSettings();
	const QString folder = settings->value("Save/path").toString();
	const QString filename = settings->value("Save/filename").toString();
	if (folder.isEmpty() || filename.isEmpty()) {
		Toast::show(page, QObject::tr("Choose a download folder and file name in Settings first."), 3500);
		return 0;
	}
	if (queue == nullptr || images.isEmpty()) {
		return 0;
	}
	auto progress = std::make_shared<QPair<int, int>>(0, 0); // finished, saved
	const int total = int(images.size());
	QPointer<QWidget> target(page);
	for (const auto &image : images) {
		auto *downloader = new ImageDownloader(profile, image, filename, folder, 1, true, true, queue);
		QObject::connect(downloader, &ImageDownloader::saved, page, [progress, total, target, folder](const QSharedPointer<Image> &, const QList<ImageSaveResult> &results) {
			++progress->first;
			for (const auto &result : results) {
				if (result.result != Image::SaveResult::Error && result.result != Image::SaveResult::NetworkError
					&& result.result != Image::SaveResult::NotFound && result.result != Image::SaveResult::NotLoaded
					&& result.result != Image::SaveResult::DetailsLoadError && result.result != Image::SaveResult::Blacklisted) {
					++progress->second;
					break;
				}
			}
			if (progress->first == total && target) {
				Toast::show(target, progress->second == total
					? QObject::tr("Saved %n picture(s) to %1", "", total).arg(QDir::toNativeSeparators(folder))
					: QObject::tr("Saved %1 of %2 pictures. See Downloads for details.").arg(progress->second).arg(total), 3200);
			}
		});
		queue->add(DownloadQueue::Manual, downloader);
	}
	Toast::show(page, QObject::tr("Downloading %n picture(s)…", "", total));
	return total;
}

bool PictureActions::toggleRating(LibraryStore *store, const QString &key, qint64 scope, bool favorite, bool *ok)
{
	bool success = !key.isEmpty() && store->contains(key);
	if (success && scope > 0 && !store->contains(key, scope)) {
		success = store->addToCollection(key, scope);
	}
	bool on = false;
	if (success) {
		const auto entry = store->entry(key, scope);
		on = favorite ? !entry.favorite : !entry.liked;
		success = favorite ? store->setFavorite(key, on, scope) : store->setLiked(key, on, scope);
	}
	if (ok != nullptr) {
		*ok = success;
	}
	return success && on;
}
