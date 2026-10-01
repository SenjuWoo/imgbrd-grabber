#include "image-library-actions.h"
#include <QAction>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QToolButton>
#include "models/image.h"
#include "models/library-store.h"
#include "models/profile.h"


ImageLibraryActions::ImageLibraryActions(Profile *profile, const QSharedPointer<Image> &image, QWidget *parent, qint64 collection, bool compact)
	: QWidget(parent), m_profile(profile), m_store(profile->library()), m_compact(compact)
{
	setObjectName("imageLibraryActions");
	auto *layout = new QHBoxLayout(this);
	layout->setContentsMargins(0, 2, 0, 2);
	layout->setSpacing(4);
	m_like = new QAction(tr("Like"), this);
	m_like->setCheckable(true);
	m_like->setObjectName("libraryLikeAction");
	m_favorite = new QAction(tr("Favorite"), this);
	m_favorite->setCheckable(true);
	m_favorite->setObjectName("libraryFavoriteAction");
	m_save = new QAction(tr("Save to Library"), this);
	m_likeButton = new QToolButton(this);
	m_likeButton->setObjectName("libraryLike");
	m_likeButton->setDefaultAction(m_like);
	m_favoriteButton = new QToolButton(this);
	m_favoriteButton->setObjectName("libraryFavorite");
	m_favoriteButton->setDefaultAction(m_favorite);
	m_collectButton = new QToolButton(this);
	m_collectButton->setObjectName("libraryCollect");
	m_collectButton->setPopupMode(QToolButton::InstantPopup);
	auto *menu = new QMenu(m_collectButton);
	connect(menu, &QMenu::aboutToShow, this, [this, menu]() { populateCollections(menu); });
	m_collectButton->setMenu(menu);
	m_scopeLabel = new QLabel(this);
	m_scopeLabel->setObjectName("libraryScope");
	m_scopeLabel->setTextFormat(Qt::PlainText);
	m_scopeLabel->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
	m_scopeLabel->setMaximumWidth(260);
	m_scopeLabel->setVisible(!compact);
	layout->addWidget(m_likeButton);
	layout->addWidget(m_favoriteButton);
	layout->addWidget(m_collectButton);
	layout->addWidget(m_scopeLabel);
	layout->addStretch();
	for (auto *button : { m_likeButton, m_favoriteButton, m_collectButton }) {
		button->setAutoRaise(true);
		button->setCursor(Qt::PointingHandCursor);
		button->setMinimumHeight(28);
	}
	setStyleSheet("QToolButton { padding: 4px 7px; border-radius: 5px; } "
		"#libraryLike:checked { color: #e75f91; background: rgba(231,95,145,30); } "
		"#libraryFavorite:checked { color: #e4ad43; background: rgba(228,173,67,30); }");
	connect(m_like, &QAction::triggered, this, [this](bool checked) {
		if (saveSelection()) {
			for (const auto &key : m_keys) {
				if (!m_store->setLiked(key, checked, m_collection)) {
					showError();
					break;
				}
			}
		}
		refresh();
	});
	connect(m_favorite, &QAction::triggered, this, [this](bool checked) {
		if (saveSelection()) {
			for (const auto &key : m_keys) {
				if (!m_store->setFavorite(key, checked, m_collection)) {
					showError();
					break;
				}
			}
		}
		refresh();
	});
	connect(m_save, &QAction::triggered, this, [this]() { saveSelection(); });
	connect(m_store, &LibraryStore::imageChanged, this, [this](const QString &key) {
		if (key.isEmpty() || m_keys.contains(key)) {
			refresh();
		}
	});
	connect(m_store, &LibraryStore::collectionsChanged, this, &ImageLibraryActions::refresh);
	setImage(image, collection);
}

void ImageLibraryActions::setImage(const QSharedPointer<Image> &image, qint64 collection)
{
	setSelection({ image }, { image ? m_store->keyForImage(*image) : QString() }, collection);
}

void ImageLibraryActions::setSelection(const QList<QSharedPointer<Image>> &images, const QStringList &keys, qint64 collection)
{
	for (const auto &connection : m_imageConnections) {
		disconnect(connection);
	}
	m_imageConnections.clear();
	m_images = images;
	m_keys = keys;
	m_keys.removeAll(QString());
	m_collection = collection;
	for (const auto &image : images) {
		if (!image) {
			continue;
		}
		auto updateSavedImage = [this, imagePointer = image.data()]() {
			if (m_store->contains(m_store->keyForImage(*imagePointer))) {
				m_store->saveImage(*imagePointer);
			}
		};
		m_imageConnections.append(connect(image.data(), &Image::finishedLoadingPreview, this, updateSavedImage));
		m_imageConnections.append(connect(image.data(), &Image::finishedLoadingTags, this, updateSavedImage));
	}
	refresh();
}

bool ImageLibraryActions::saveSelection()
{
	for (const auto &image : m_images) {
		if (image && m_store->saveImage(*image).isEmpty()) {
			showError();
			return false;
		}
	}
	for (const auto &key : m_keys) {
		if (!m_store->contains(key, m_collection)) {
			showError();
			return false;
		}
	}
	return !m_keys.isEmpty();
}

void ImageLibraryActions::showError()
{
	QMessageBox::warning(this, tr("Library"), m_store->lastError());
}

void ImageLibraryActions::refresh()
{
	// An explicit source link can merge a source entry while its viewer is open.
	if (m_images.size() == m_keys.size()) {
		for (int i = 0; i < m_images.size(); ++i) {
			if (m_images[i]) {
				m_keys[i] = m_store->keyForImage(*m_images[i]);
			}
		}
	}
	QString scope = tr("Library-wide");
	bool scopeExists = m_collection == 0;
	if (m_collection > 0) {
		for (const auto &collection : m_store->collections()) {
			if (collection.id == m_collection) {
				scope = collection.name;
				scopeExists = true;
				break;
			}
		}
		if (!scopeExists) {
			scope = tr("Collection removed");
		}
	}
	const bool enabled = m_store->isReady() && scopeExists && !m_keys.isEmpty();
	bool liked = enabled;
	bool favorite = enabled;
	for (const auto &key : m_keys) {
		const auto state = m_store->entry(key, m_collection);
		liked = liked && state.liked;
		favorite = favorite && state.favorite;
	}
	m_like->setChecked(liked);
	m_favorite->setChecked(favorite);
	m_like->setEnabled(enabled);
	m_favorite->setEnabled(enabled);
	m_save->setEnabled(enabled);
	m_collectButton->setEnabled(enabled);
	m_likeButton->setText(m_compact ? (liked ? QStringLiteral("♥") : QStringLiteral("♡")) : tr("♥ Like"));
	m_favoriteButton->setText(m_compact ? (favorite ? QStringLiteral("★") : QStringLiteral("☆")) : tr("★ Favorite"));
	m_collectButton->setText(m_compact ? QStringLiteral("+") : tr("+ Collection"));
	m_likeButton->setToolTip(tr("Like — %1").arg(scope));
	m_favoriteButton->setToolTip(tr("Favorite — %1").arg(scope));
	m_collectButton->setToolTip(tr("Save to Library or organize into collections"));
	m_likeButton->setAccessibleName(m_likeButton->toolTip());
	m_favoriteButton->setAccessibleName(m_favoriteButton->toolTip());
	m_collectButton->setAccessibleName(m_collectButton->toolTip());
	m_scopeLabel->setText(scope);
	m_scopeLabel->setToolTip(tr("Likes and favorites apply only to %1.").arg(scope));
	if (!m_store->isReady()) {
		setToolTip(m_store->lastError());
	}
}

void ImageLibraryActions::populateCollections(QMenu *menu)
{
	menu->clear();
	menu->addAction(m_save);
	menu->addSeparator();
	for (const auto &collection : m_store->collections()) {
		auto *action = menu->addAction(collection.name);
		action->setCheckable(true);
		bool allMembers = !m_keys.isEmpty();
		for (const auto &key : m_keys) {
			allMembers = allMembers && m_store->contains(key, collection.id);
		}
		action->setChecked(allMembers);
		connect(action, &QAction::triggered, this, [this, id = collection.id](bool checked) {
			// Membership edits use Library-wide existence, independent of the rating scope.
			for (const auto &image : m_images) {
				if (image && m_store->saveImage(*image).isEmpty()) {
					showError();
					return;
				}
			}
			for (const auto &key : m_keys) {
				if (!(checked ? m_store->addToCollection(key, id) : m_store->removeFromCollection(key, id))) {
					showError();
					return;
				}
			}
		});
	}
	menu->addSeparator();
	menu->addAction(tr("New collection…"), this, [this]() {
		bool accepted;
		const QString name = QInputDialog::getText(this, tr("New collection"), tr("Name:"), QLineEdit::Normal, QString(), &accepted);
		if (!accepted) {
			return;
		}
		const qint64 id = m_store->createCollection(name);
		if (id == 0) {
			showError();
			return;
		}
		for (const auto &image : m_images) {
			if (image && m_store->saveImage(*image).isEmpty()) {
				showError();
				return;
			}
		}
		for (const auto &key : m_keys) {
			if (!m_store->addToCollection(key, id)) {
				showError();
				return;
			}
		}
	});
}

void ImageLibraryActions::addToMenu(QMenu *menu)
{
	menu->addAction(m_like);
	menu->addAction(m_favorite);
	menu->addAction(m_save);
	auto *collections = menu->addMenu(tr("Collections"));
	populateCollections(collections);
	collections->setEnabled(m_collectButton->isEnabled());
	menu->addSeparator();
}
