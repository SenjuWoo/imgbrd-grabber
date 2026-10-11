#include "picture-actions.h"
#include <QCheckBox>
#include <QDir>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
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

void PictureActions::offerRemoval(Profile *profile, const QStringList &keys, QWidget *parent)
{
	LibraryStore *store = profile->library();
	QStringList unrated;
	const QStringList all = store->unratedKeys();
	for (const QString &key : keys) {
		if (all.contains(key) && !unrated.contains(key)) {
			unrated.append(key);
		}
	}
	if (unrated.isEmpty()) {
		return;
	}
	// Context menus close (and may be deleted) right after their action: ask from the window behind them.
	QWidget *owner = parent != nullptr ? parent->window() : nullptr;
	while (owner != nullptr && owner->windowFlags().testFlag(Qt::Popup)) {
		owner = owner->parentWidget() != nullptr ? owner->parentWidget()->window() : nullptr;
	}
	auto remove = [store, unrated, parent = QPointer<QWidget>(owner)]() {
		int removed = 0;
		for (const QString &key : unrated) {
			removed += store->removeImage(key) ? 1 : 0;
		}
		if (parent) {
			Toast::show(parent, QObject::tr("Removed %n picture(s) from Library", "", removed));
		}
	};
	const int remembered = profile->getSettings()->value("Library/removeUnrated", 0).toInt();
	if (remembered == 1) {
		remove();
		return;
	}
	if (remembered == 2) {
		return;
	}
	auto *box = new QMessageBox(QMessageBox::Question, QObject::tr("Remove from Library?"),
		QObject::tr("%n picture(s) are no longer liked or favorited. Remove them from your Library too?", "", int(unrated.size())), QMessageBox::NoButton, owner);
	box->setAttribute(Qt::WA_DeleteOnClose);
	auto *removeButton = box->addButton(QObject::tr("Remove"), QMessageBox::AcceptRole);
	box->addButton(QObject::tr("Keep"), QMessageBox::RejectRole);
	box->setDefaultButton(removeButton);
	box->setCheckBox(new QCheckBox(QObject::tr("Remember my choice"), box));
	QObject::connect(box, &QMessageBox::finished, box, [box, removeButton, remove, settings = profile->getSettings()]() {
		const bool removing = box->clickedButton() == removeButton;
		if (box->checkBox()->isChecked()) {
			settings->setValue("Library/removeUnrated", removing ? 1 : 2);
		}
		if (removing) {
			remove();
		}
	});
	box->open();
}
