#include "picture-actions.h"
#include <QCheckBox>
#include <QDir>
#include <QInputDialog>
#include <QMenu>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QSettings>
#include <QWidget>
#include <memory>
#include "discovery-feed.h"
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

void PictureActions::fillNotInterested(QMenu *menu, DiscoveryFeed *feed, const QSharedPointer<Image> &image, int count, const std::function<void(const QStringList &, bool)> &act, QWidget *parent)
{
	menu->addAction(count == 1 ? QObject::tr("Hide this picture") : QObject::tr("Hide these %n pictures", "", count), menu, [act]() { act({}, false); });
	if (!image || feed == nullptr) {
		return;
	}
	// The tags that match this taste, then who drew it and what it shows.
	QStringList choices = feed->taste().matches(image->tagsString(), 6);
	for (const auto &tag : image->tags()) {
		const QString type = tag.type().name();
		const QString name = TasteProfile::normalize(tag.text());
		if ((type == QLatin1String("artist") || type == QLatin1String("character") || type == QLatin1String("copyright")) && !choices.contains(name) && choices.size() < 9) {
			choices.append(name);
		}
	}
	QStringList all;
	for (const QString &tag : image->tagsString()) {
		const QString name = TasteProfile::normalize(tag);
		if (!name.isEmpty() && !TasteProfile::isMetaTag(name)) {
			all.append(name);
		}
	}
	if (choices.isEmpty()) {
		choices = all.mid(0, 6);
	}
	menu->addSection(QObject::tr("Show less of…"));
	for (const QString &tag : choices) {
		menu->addAction(QString(tag).replace('_', ' '), menu, [act, tag]() { act({tag}, false); });
	}
	auto *never = menu->addMenu(QObject::tr("Never show…"));
	for (const QString &tag : choices) {
		never->addAction(QString(tag).replace('_', ' '), never, [act, tag]() { act({tag}, true); });
	}
	menu->addAction(QObject::tr("Another tag…"), menu, [act, all, parent = QPointer<QWidget>(parent)]() {
		bool ok = false;
		const QString tag = QInputDialog::getItem(parent, QObject::tr("Not interested"), QObject::tr("Show less of this tag:"), all, 0, true, &ok).trimmed();
		if (ok && !tag.isEmpty()) {
			act({tag}, false);
		}
	});
}

QString PictureActions::notInterestedMessage(const QStringList &tags, bool never)
{
	return tags.isEmpty() ? QObject::tr("Hidden. You'll see fewer pictures like this.")
		: never ? QObject::tr("Hidden. You won't see %1 again.").arg(tags.join(", ")) : QObject::tr("Hidden. You'll see less %1.").arg(tags.join(", "));
}
