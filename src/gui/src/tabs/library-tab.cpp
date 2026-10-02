#include "tabs/library-tab.h"
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QFile>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QProgressDialog>
#include <QSettings>
#include <QtConcurrent>
#include "models/library-importer.h"
#include "viewer/library-image-dialog.h"
#include "viewer/library-source-dialog.h"
#include <QHBoxLayout>
#include <QInputDialog>
#include <QImageReader>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStackedWidget>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#include "image-context-menu.h"
#include "image-library-actions.h"
#include "main-window.h"
#include "models/image.h"
#include "models/profile.h"
#include "viewer/viewer-window.h"


namespace { constexpr int LibraryPageSize = 100; }

LibraryTab::LibraryTab(Profile *profile, MainWindow *parent)
	: QWidget(parent), m_profile(profile), m_mainWindow(parent), m_store(profile->library())
{
	setObjectName("libraryTab");
	setAcceptDrops(true);
	m_copyImports = profile->getSettings()->value("Library/copyImports", false).toBool();
	setWindowTitle(tr("Library"));
	setMaximumWidth(16777214); // Existing convention for permanent tabs.
	auto *layout = new QHBoxLayout(this);
	layout->setContentsMargins(16, 16, 16, 16);
	auto *splitter = new QSplitter(this);
	layout->addWidget(splitter);
	auto *sidebar = new QWidget(splitter);
	auto *sideLayout = new QVBoxLayout(sidebar);
	sideLayout->setContentsMargins(0, 0, 12, 0);
	auto *heading = new QLabel(tr("Your library"), sidebar);
	QFont headingFont = heading->font();
	headingFont.setPointSize(16);
	headingFont.setBold(true);
	heading->setFont(headingFont);
	sideLayout->addWidget(heading);
	m_sidebar = new QTreeWidget(sidebar);
	m_sidebar->setObjectName("librarySidebar");
	m_sidebar->setHeaderHidden(true);
	m_sidebar->setRootIsDecorated(false);
	m_sidebar->setIndentation(12);
	m_sidebar->setIconSize(QSize(36, 36));
	m_sidebar->setMinimumWidth(180);
	m_sidebar->setContextMenuPolicy(Qt::CustomContextMenu);
	sideLayout->addWidget(m_sidebar, 1);
	auto *create = new QPushButton(tr("+ New collection"), sidebar);
	create->setObjectName("libraryNewCollection");
	create->setEnabled(m_store->isReady());
	sideLayout->addWidget(create);
	m_manage = new QPushButton(tr("Manage collection…"), sidebar);
	m_manage->setObjectName("libraryManageCollection");
	sideLayout->addWidget(m_manage);
	auto *content = new QWidget(splitter);
	auto *contentLayout = new QVBoxLayout(content);
	contentLayout->setContentsMargins(8, 0, 0, 0);
	m_title = new QLabel(content);
	m_title->setTextFormat(Qt::PlainText);
	m_title->setWordWrap(true);
	QFont titleFont = m_title->font();
	titleFont.setPointSize(20);
	titleFont.setBold(true);
	m_title->setFont(titleFont);
	contentLayout->addWidget(m_title);
	m_hint = new QLabel(content);
	m_hint->setTextFormat(Qt::PlainText);
	m_hint->setWordWrap(true);
	contentLayout->addWidget(m_hint);
	auto *filters = new QHBoxLayout();
	m_search = new QLineEdit(content);
	m_search->setObjectName("librarySearch");
	m_search->setPlaceholderText(tr("Search titles, tags, sources and notes"));
	m_search->setClearButtonEnabled(true);
	m_filter = new QComboBox(content);
	m_filter->setObjectName("libraryFilter");
	m_filter->addItems({ tr("All pictures"), tr("Liked"), tr("Favorites") });
	filters->addWidget(m_search, 1);
	filters->addWidget(m_filter);
	m_importButton = new QPushButton(tr("Import pictures…"), content);
	m_importButton->setObjectName("libraryImport");
	m_importButton->setEnabled(m_store->isReady());
	m_importButton->setToolTip(tr("Choose files or a folder, or drop pictures into Library."));
	auto *importMenu = new QMenu(m_importButton);
	importMenu->addAction(tr("Choose pictures…"), this, [this]() {
		QStringList patterns;
		for (const auto &format : QImageReader::supportedImageFormats()) {
			patterns.append("*." + QString::fromLatin1(format));
		}
		const auto files = QFileDialog::getOpenFileNames(this, tr("Import pictures"), QString(), tr("Pictures (%1);;All files (*)").arg(patterns.join(' ')));
		if (!files.isEmpty()) {
			importPaths(files, m_copyImports);
		}
	});
	importMenu->addAction(tr("Choose folder…"), this, [this]() {
		const auto folder = QFileDialog::getExistingDirectory(this, tr("Import folder and subfolders"));
		if (!folder.isEmpty()) {
			importPaths({folder}, m_copyImports);
		}
	});
	importMenu->addSeparator();
	importMenu->addAction(tr("Recheck metadata for selected pictures"), this, [this]() {
		QStringList paths;
		for (auto *item : m_grid->selectedItems()) {
			for (const auto &path : m_entries.value(item->data(Qt::UserRole).toString()).localPaths) {
				if (QFile::exists(path)) { paths.append(path); }
			}
		}
		paths.removeDuplicates();
		importPaths(paths);
	});
	importMenu->addAction(tr("Recheck metadata in this view"), this, [this]() {
		QStringList paths;
		for (const auto &key : m_viewKeys) {
			for (const auto &path : m_entries.value(key).localPaths) {
				if (QFile::exists(path)) { paths.append(path); }
			}
		}
		paths.removeDuplicates();
		importPaths(paths);
	});
	importMenu->addSeparator();
	auto *copyAction = importMenu->addAction(tr("Copy into portable Library"));
	copyAction->setCheckable(true);
	copyAction->setChecked(m_copyImports);
	copyAction->setToolTip(tr("Off: reference existing files. On: keep an extra copy beside your Library catalog. Originals stay on disk."));
	connect(copyAction, &QAction::toggled, this, [this](bool copy) {
		m_copyImports = copy;
		m_profile->getSettings()->setValue("Library/copyImports", copy);
	});
	m_importButton->setMenu(importMenu);
	filters->addWidget(m_importButton);
	contentLayout->addLayout(filters);
	m_count = new QLabel(content);
	auto *pageRow = new QHBoxLayout();
	pageRow->addWidget(m_count, 1);
	m_previousPage = new QPushButton(tr("Previous page"), content);
	m_previousPage->setObjectName("libraryPreviousPage");
	m_nextPage = new QPushButton(tr("Next page"), content);
	m_nextPage->setObjectName("libraryNextPage");
	m_pageLabel = new QLabel(content);
	m_pageLabel->setObjectName("libraryPage");
	pageRow->addWidget(m_previousPage);
	pageRow->addWidget(m_pageLabel);
	pageRow->addWidget(m_nextPage);
	contentLayout->addLayout(pageRow);
	connect(m_previousPage, &QPushButton::clicked, this, [this]() { if (m_page > 0) { --m_page; reload(); } });
	connect(m_nextPage, &QPushButton::clicked, this, [this]() { if ((m_page + 1) * LibraryPageSize < m_viewKeys.size()) { ++m_page; reload(); } });
	m_stack = new QStackedWidget(content);
	m_grid = new QListWidget(m_stack);
	m_grid->setObjectName("libraryGrid");
	m_grid->setViewMode(QListView::IconMode);
	m_grid->setResizeMode(QListView::Adjust);
	m_grid->setMovement(QListView::Static);
	m_grid->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
	m_grid->setIconSize(QSize(224, 160));
	m_grid->setGridSize(QSize(248, 222));
	m_grid->setSpacing(8);
	m_grid->setWordWrap(false);
	m_grid->setUniformItemSizes(true);
	m_grid->setSelectionMode(QAbstractItemView::ExtendedSelection);
	m_grid->setContextMenuPolicy(Qt::CustomContextMenu);
	m_grid->setFrameShape(QFrame::NoFrame);
	m_grid->setTextElideMode(Qt::ElideRight);
	m_stack->addWidget(m_grid);
	m_empty = new QLabel(m_stack);
	m_empty->setAlignment(Qt::AlignCenter);
	m_empty->setWordWrap(true);
	m_empty->setMargin(32);
	m_stack->addWidget(m_empty);
	contentLayout->addWidget(m_stack, 1);
	m_actions = new ImageLibraryActions(profile, {}, content);
	auto *actionRow = new QHBoxLayout();
	actionRow->addWidget(m_actions, 1);
	m_findSource = new QPushButton(tr("Find / link source…"), content);
	m_findSource->setObjectName("libraryFindSource");
	m_findSource->setToolTip(tr("Select one picture. Search an exact MD5 on a website, or compare source pictures already saved in Library."));
	actionRow->addWidget(m_findSource);
	connect(m_findSource, &QPushButton::clicked, this, [this]() {
		if (m_grid->selectedItems().size() == 1) { findSource(m_grid->selectedItems().first()->data(Qt::UserRole).toString()); }
	});
	m_more = new QPushButton(tr("More…"), content);
	m_more->setObjectName("libraryMore");
	actionRow->addWidget(m_more);
	contentLayout->addLayout(actionRow);
	splitter->setStretchFactor(0, 0);
	splitter->setStretchFactor(1, 1);
	splitter->setSizes({ 220, 1000 });
	setStyleSheet("#librarySidebar { border: 0; background: transparent; } "
		"#librarySidebar::item { padding: 9px 6px; border-radius: 6px; } "
		"#libraryGrid::item { padding: 8px; border-radius: 8px; } "
		"#librarySearch { padding: 8px; border-radius: 6px; }");
	connect(create, &QPushButton::clicked, this, &LibraryTab::newCollection);
	connect(m_manage, &QPushButton::clicked, this, [this]() {
		if (auto *item = m_sidebar->currentItem()) {
			m_sidebar->scrollToItem(item);
			collectionMenu(m_sidebar->visualItemRect(item).center());
		}
	});
	connect(m_more, &QPushButton::clicked, this, [this]() {
		if (!m_grid->selectedItems().isEmpty()) {
			auto *item = m_grid->selectedItems().first();
			m_grid->scrollToItem(item);
			imageMenu(m_grid->visualItemRect(item).center());
		}
	});
	connect(m_store, &LibraryStore::imageChanged, this, &LibraryTab::scheduleReload);
	connect(m_store, &LibraryStore::collectionsChanged, this, &LibraryTab::scheduleReload);
	m_searchTimer = new QTimer(this);
	m_searchTimer->setSingleShot(true);
	m_searchTimer->setInterval(150);
	connect(m_searchTimer, &QTimer::timeout, this, &LibraryTab::scheduleReload);
	connect(m_search, &QLineEdit::textChanged, this, [this]() { m_page = 0; m_searchTimer->start(); });
	connect(m_filter, &QComboBox::currentIndexChanged, this, [this]() { m_page = 0; scheduleReload(); });
	connect(m_sidebar, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem *item) {
		if (item != nullptr && item->data(0, Qt::UserRole).isValid()) {
			m_page = 0;
			m_collection = item->data(0, Qt::UserRole).toLongLong();
			m_smartFilter = item->data(0, Qt::UserRole + 1).toInt();
			m_filter->setCurrentIndex(0);
			scheduleReload();
		}
	});
	connect(m_sidebar, &QTreeWidget::customContextMenuRequested, this, &LibraryTab::collectionMenu);
	connect(m_grid, &QListWidget::itemSelectionChanged, this, &LibraryTab::updateSelection);
	connect(m_grid, &QListWidget::itemActivated, this, [this](QListWidgetItem *item) { openImage(item->data(Qt::UserRole).toString()); });
	connect(m_grid, &QListWidget::customContextMenuRequested, this, &LibraryTab::imageMenu);
	reload();
}

void LibraryTab::scheduleReload()
{
	if (m_reloadPending || m_importing) {
		return;
	}
	m_reloadPending = true;
	// Queue refreshes so editing an action cannot delete its sender mid-click.
	QTimer::singleShot(0, this, [this]() { m_reloadPending = false; reload(); });
}

void LibraryTab::reload()
{
	if (m_importing) {
		return;
	}
	const auto collections = m_store->collections();
	bool collectionExists = m_collection == 0;
	QString title = tr("All pictures");
	for (const auto &collection : collections) {
		if (collection.id == m_collection) {
			title = collection.name;
			collectionExists = true;
		}
	}
	if (!collectionExists) {
		m_collection = 0;
		m_smartFilter = 0;
	}
	QStringList selected;
	for (auto *item : m_grid->selectedItems()) {
		selected.append(item->data(Qt::UserRole).toString());
	}
	const QSignalBlocker sidebarBlock(m_sidebar);
	const QSignalBlocker gridBlock(m_grid);
	m_sidebar->clear();
	const QStringList smartNames { tr("All pictures"), tr("Unsorted"), tr("Liked"), tr("Favorites"), tr("Recently saved"), tr("Needs tags"), tr("Needs source"), tr("Metadata errors") };
	for (int index = 0; index < smartNames.size(); ++index) {
		auto *item = new QTreeWidgetItem(m_sidebar, { smartNames[index] });
		item->setData(0, Qt::UserRole, 0);
		item->setData(0, Qt::UserRole + 1, index);
		if (m_collection == 0 && m_smartFilter == index) {
			m_sidebar->setCurrentItem(item);
			title = smartNames[index];
		}
	}
	auto *group = new QTreeWidgetItem(m_sidebar, { tr("Collections") });
	group->setFlags(Qt::ItemIsEnabled);
	group->setExpanded(true);
	for (const auto &collection : collections) {
		auto *item = new QTreeWidgetItem(group, { tr("%1 (%2)").arg(collection.name).arg(collection.count) });
		item->setData(0, Qt::UserRole, collection.id);
		QPixmap cover;
		cover.loadFromData(collection.cover);
		if (!cover.isNull()) {
			item->setIcon(0, QIcon(cover.scaled(36, 36, Qt::KeepAspectRatio, Qt::SmoothTransformation)));
		}
		if (collection.id == m_collection) {
			m_sidebar->setCurrentItem(item);
		}
	}
	m_title->setText(title);
	m_hint->setText(m_collection > 0 ? tr("Likes, favorites and notes apply to this collection. A picture can belong to several collections.")
		: tr("Library-wide likes and favorites. Organize pictures into collections for separate preferences."));
	m_grid->clear();
	m_entries.clear();
	m_viewKeys.clear();
	const QString search = m_search->text().trimmed();
	m_manage->setEnabled(m_store->isReady() && m_collection > 0);
	const int preference = m_filter->currentIndex();
	const auto entries = m_store->entries(m_collection);
	// ponytail: read metadata in one pass, render 100 cards; query pages if catalogs exceed tens of thousands of pictures.
	for (const auto &entry : entries) {
		if ((m_collection == 0 && m_smartFilter == 1 && entry.collectionCount > 0) || (m_collection == 0 && m_smartFilter == 2 && !entry.liked) || (m_collection == 0 && m_smartFilter == 3 && !entry.favorite) || (preference == 1 && !entry.liked) || (preference == 2 && !entry.favorite)) {
			continue;
		}
		if (m_collection == 0 && m_smartFilter == 4 && QDateTime::fromString(entry.savedAt, Qt::ISODateWithMs) < QDateTime::currentDateTimeUtc().addDays(-7)) {
			continue;
		}
		if (m_collection == 0 && ((m_smartFilter == 5 && !entry.tags().isEmpty()) || (m_smartFilter == 6 && !entry.image.value("website").toString().isEmpty()) || (m_smartFilter == 7 && entry.metadataErrors().isEmpty()))) {
			continue;
		}
		const QString searchable = QString::fromUtf8(QJsonDocument(entry.image).toJson(QJsonDocument::Compact)) + " " + entry.notes;
		bool matches = true;
		for (const auto &word : search.split(' ', Qt::SkipEmptyParts)) {
			matches = matches && searchable.contains(word, Qt::CaseInsensitive);
		}
		if (!matches) {
			continue;
		}
		m_entries.insert(entry.key, entry);
		m_viewKeys.append(entry.key);
	}
	m_page = qBound(0, m_page, qMax(0, int((m_viewKeys.size() - 1) / LibraryPageSize)));
	m_previousPage->setEnabled(m_page > 0 && !m_importing);
	m_nextPage->setEnabled((m_page + 1) * LibraryPageSize < m_viewKeys.size() && !m_importing);
	m_pageLabel->setText(tr("Page %1 of %2").arg(m_page + 1).arg(qMax(1, int((m_viewKeys.size() + LibraryPageSize - 1) / LibraryPageSize))));
	const bool paged = m_viewKeys.size() > LibraryPageSize;
	m_previousPage->setVisible(paged);
	m_nextPage->setVisible(paged);
	m_pageLabel->setVisible(paged);
	if (m_collection == 0 && m_smartFilter == 5) {
		m_hint->setText(tr("No tags are available yet. The picture imported successfully; tags may never have been saved in the file. Likes and favorites still work. Select a picture and use Find / link source, or recheck metadata from Import pictures."));
	} else if (m_collection == 0 && m_smartFilter == 6) {
		m_hint->setText(tr("These pictures have no identified website post. Recovered file tags are kept separately from source identity. Find / link source offers exact MD5 lookup and similar cached source pictures."));
	} else if (m_collection == 0 && m_smartFilter == 7) {
		m_hint->setText(tr("The picture was imported, but one or more metadata readers reported an error. Open the picture's Overview for the reason, then recheck metadata after correcting it."));
	}
	for (const auto &key : m_viewKeys.mid(m_page * LibraryPageSize, LibraryPageSize)) {
		const auto &entry = m_entries[key];
		QString name = entry.image.value("name").toString();
		if (name.isEmpty()) {
			name = entry.image.value("data").toObject().value("title").toString();
		}
		if (name.isEmpty()) {
			name = tr("Picture #%1").arg(entry.image.value("id").toString());
		}
		QString source = entry.image.value("website").toString();
		if (source.isEmpty()) {
			source = tr("Local file · source unlinked");
		}
		const QString metadataState = entry.tags().isEmpty() ? tr("Needs tags") : tr("%1 tags").arg(entry.tags().size());
		const QString warning = entry.metadataErrors().isEmpty() ? QString() : tr(" · Metadata error");
		QString badges = (entry.liked ? QStringLiteral("♥ ") : QString()) + (entry.favorite ? QStringLiteral("★ ") : QString());
		if (!entry.notes.isEmpty()) {
			badges += tr("Note · ");
		}
		auto *item = new QListWidgetItem(name + "\n" + source + " · " + metadataState + warning + "\n" + badges + (entry.collectionCount == 1 ? tr("1 collection") : tr("%1 collections").arg(entry.collectionCount)), m_grid);
		item->setData(Qt::UserRole, entry.key);
		item->setToolTip(name + "\n" + source + "\n" + metadataState + "\n" + entry.tags().join(", ") + "\n" + entry.metadataErrors().join("\n") + "\n" + entry.notes);
		QPixmap thumbnail;
		thumbnail.loadFromData(entry.thumbnail);
		if (thumbnail.isNull()) {
			thumbnail.load(":/images/noimage.png");
		}
		QIcon icon(thumbnail);
		icon.addPixmap(thumbnail, QIcon::Selected);
		item->setIcon(icon);
		item->setTextAlignment(Qt::AlignHCenter);
		item->setSelected(selected.contains(entry.key));
	}

	if (!m_store->isReady()) {
		m_empty->setText(m_store->lastError());
	} else if (!search.isEmpty() || preference > 0) {
		m_empty->setText(tr("No pictures match these filters."));
	} else if (m_collection > 0) {
		m_empty->setText(tr("This collection is empty.\nUse + Collection on a picture in search, the viewer or Library to add it."));
	} else {
		m_empty->setText(tr("Keep the pictures you want to find again.\nUse ♥ Like, ★ Favorite or + Collection on any search result or in the viewer."));
	}
	m_stack->setCurrentWidget(m_grid->count() > 0 ? static_cast<QWidget*>(m_grid) : static_cast<QWidget*>(m_empty));
	updateSelection();
}

QSharedPointer<Image> LibraryTab::restoreImage(const LibraryEntry &entry)
{
	if (entry.key.isEmpty() || !m_profile->getSites().contains(entry.image.value("website").toString())) {
		return {};
	}
	auto image = QSharedPointer<Image>::create(m_profile);
	if (!image->read(entry.image, m_profile->getSites())) {
		return {};
	}
	QPixmap thumbnail;
	thumbnail.loadFromData(entry.thumbnail);
	if (!thumbnail.isNull()) {
		image->setPreviewImage(thumbnail);
	}
	for (const QString &path : entry.localPaths + (image->md5().isEmpty() ? QStringList() : m_profile->md5Exists(image->md5()))) {
		if (QFile::exists(path)) {
			image->setSavePath(path);
			break;
		}
	}
	return image;
}

void LibraryTab::updateSelection()
{
	QStringList keys;
	QList<QSharedPointer<Image>> images;
	for (auto *item : m_grid->selectedItems()) {
		const QString key = item->data(Qt::UserRole).toString();
		keys.append(key);
		images.append(restoreImage(m_entries.value(key)));
	}
	m_actions->setSelection(images, keys, m_collection);
	m_more->setEnabled(!keys.isEmpty());
	m_findSource->setEnabled(keys.size() == 1 && !m_importing);
	const int total = m_viewKeys.size();
	const QString count = total > LibraryPageSize ? tr("%1–%2 of %3 pictures").arg(m_page * LibraryPageSize + 1).arg(m_page * LibraryPageSize + m_grid->count()).arg(total)
		: (total == 1 ? tr("1 picture") : tr("%1 pictures").arg(total));
	m_count->setText(count + (keys.isEmpty() ? QString() : tr(" · %1 selected").arg(keys.size())));
}

void LibraryTab::newCollection()
{
	bool accepted;
	const QString name = QInputDialog::getText(this, tr("New collection"), tr("Name:"), QLineEdit::Normal, QString(), &accepted);
	if (!accepted) {
		return;
	}
	const qint64 id = m_store->createCollection(name);
	if (id == 0) {
		QMessageBox::warning(this, tr("Library"), m_store->lastError());
		return;
	}
	m_collection = id;
	m_smartFilter = 0;
	m_search->clear();
	m_filter->setCurrentIndex(0);
	scheduleReload();
}

void LibraryTab::collectionMenu(const QPoint &pos)
{
	auto *item = m_sidebar->itemAt(pos);
	if (item == nullptr || item->data(0, Qt::UserRole).toLongLong() <= 0) {
		return;
	}
	const qint64 id = item->data(0, Qt::UserRole).toLongLong();
	QString name;
	for (const auto &collection : m_store->collections()) {
		if (collection.id == id) {
			name = collection.name;
		}
	}
	QMenu menu(this);
	menu.addAction(tr("Rename collection…"), this, [this, id, name]() {
		bool accepted;
		const QString renamed = QInputDialog::getText(this, tr("Rename collection"), tr("Name:"), QLineEdit::Normal, name, &accepted);
		if (accepted && !m_store->renameCollection(id, renamed)) {
			QMessageBox::warning(this, tr("Library"), m_store->lastError());
		}
	});
	menu.addAction(tr("Delete collection…"), this, [this, id, name]() {
		if (QMessageBox::question(this, tr("Delete collection"), tr("Delete %1? Its pictures remain in Library and on disk.").arg(name)) == QMessageBox::Yes
			&& !m_store->removeCollection(id)) {
			QMessageBox::warning(this, tr("Library"), m_store->lastError());
		}
	});
	menu.exec(m_sidebar->viewport()->mapToGlobal(pos));
}

void LibraryTab::openImage(const QString &key)
{
	const auto entry = m_entries.value(key);
	auto image = restoreImage(entry);
	if (!entry.localPaths.isEmpty() || !image || !image->savePath().isEmpty()) {
		const QStringList keys = m_viewKeys;
		auto *viewer = new LibraryImageDialog(m_profile, keys, key, m_collection, this);
		connect(viewer, &LibraryImageDialog::locateRequested, this, &LibraryTab::locateFile);
		connect(viewer, &LibraryImageDialog::sourceRequested, this, &LibraryTab::findSource);
		viewer->show();
		return;
	}
	if (image->isGallery() && m_mainWindow != nullptr) {
		m_mainWindow->addGalleryTab(image->parentSite(), image);
		return;
	}
	QList<QSharedPointer<Image>> images;
	for (int index = 0; index < m_grid->count(); ++index) {
		const QString otherKey = m_grid->item(index)->data(Qt::UserRole).toString();
		auto other = otherKey == key ? image : restoreImage(m_entries.value(otherKey));
		if (other && !other->isGallery()) {
			images.append(other);
		}
	}
	auto *viewer = new ViewerWindow(images, image, image->parentSite(), m_profile, m_mainWindow, nullptr, m_collection);
	viewer->show();
}

void LibraryTab::imageMenu(const QPoint &pos)
{
	auto *item = m_grid->itemAt(pos);
	if (item == nullptr) {
		return;
	}
	if (!item->isSelected()) {
		m_grid->clearSelection();
		item->setSelected(true);
	}
	const QString key = item->data(Qt::UserRole).toString();
	const auto entry = m_entries.value(key);
	QMenu menu(this);
	m_actions->addToMenu(&menu);
	if (m_grid->selectedItems().count() == 1) {
		menu.addAction(tr("View picture"), this, [this, key]() { openImage(key); });
		const QUrl page(entry.image.value("page_url").toString());
		if (page.isValid() && !page.host().isEmpty() && (page.scheme() == "http" || page.scheme() == "https")) {
			menu.addAction(tr("Open source page"), this, [page]() { QDesktopServices::openUrl(page); });
		}
		menu.addAction(tr("Find / link source…"), this, [this, key]() { findSource(key); });
		menu.addAction(tr("Locate matching local file…"), this, [this, key]() { locateFile(key); });
		menu.addAction(tr("Edit note…"), this, [this, key, entry]() {
			bool accepted;
			const QString notes = QInputDialog::getMultiLineText(this, tr("Picture note"), tr("Note for this view:"), entry.notes, &accepted);
			if (accepted && !m_store->setNotes(key, notes, m_collection)) {
				QMessageBox::warning(this, tr("Library"), m_store->lastError());
			}
		});
		if (m_collection > 0) {
			menu.addAction(tr("Use as collection cover"), this, [this, key]() {
				if (!m_store->setCollectionCover(m_collection, key)) {
					QMessageBox::warning(this, tr("Library"), m_store->lastError());
				}
			});
		}
	}
	QStringList selected;
	for (auto *selectedItem : m_grid->selectedItems()) {
		selected.append(selectedItem->data(Qt::UserRole).toString());
	}
	menu.addSeparator();
	if (m_collection > 0) {
		menu.addAction(tr("Remove from this collection"), this, [this, selected]() {
			for (const auto &selectedKey : selected) {
				if (!m_store->removeFromCollection(selectedKey, m_collection)) {
					QMessageBox::warning(this, tr("Library"), m_store->lastError());
					break;
				}
			}
		});
	}
	menu.addAction(tr("Remove from Library…"), this, [this, selected]() {
		if (QMessageBox::question(this, tr("Remove from Library"), (selected.size() == 1 ? tr("Remove this picture from Library and all collections? Downloaded files stay on disk.") : tr("Remove %1 pictures from Library and all collections? Downloaded files stay on disk.").arg(selected.size()))) != QMessageBox::Yes) {
			return;
		}
		for (const auto &selectedKey : selected) {
			if (!m_store->removeImage(selectedKey)) {
				QMessageBox::warning(this, tr("Library"), m_store->lastError());
				break;
			}
		}
	});
	menu.exec(m_grid->viewport()->mapToGlobal(pos));
}

LibraryTab::~LibraryTab()
{
	if (m_cancel) {
		m_cancel->store(true);
	}
}

void LibraryTab::dragEnterEvent(QDragEnterEvent *event)
{
	if (!m_importing && m_store->isReady() && event->mimeData()->hasUrls()) {
		for (const auto &url : event->mimeData()->urls()) {
			if (url.isLocalFile()) {
				event->acceptProposedAction(); return;
			}
		}
	}
}

void LibraryTab::dropEvent(QDropEvent *event)
{
	QStringList paths;
	for (const auto &url : event->mimeData()->urls()) {
		if (url.isLocalFile()) {
			paths.append(url.toLocalFile());
		}
	}
	if (!paths.isEmpty() && !m_importing) {
		event->acceptProposedAction();
		importPaths(paths, m_copyImports);
	}
}

void LibraryTab::locateFile(const QString &key)
{
	if (m_importing) {
		return;
	}
	const QString file = QFileDialog::getOpenFileName(this, tr("Locate the original picture"));
	if (!file.isEmpty()) {
		importPaths({file}, false, key);
	}
}

void LibraryTab::findSource(const QString &key)
{
	auto *dialog = new LibrarySourceDialog(m_profile, key, this);
	dialog->setAttribute(Qt::WA_DeleteOnClose);
	dialog->show();
}

void LibraryTab::importPaths(const QStringList &paths, bool copy, const QString &expectedKey)
{
	if (m_importing || paths.isEmpty() || !m_store->isReady()) {
		return;
	}
	m_importing = true;
	m_added = m_duplicates = m_failed = m_importIndex = 0;
	m_importErrors.clear();
	m_importFiles.clear();
	m_knownKeys.clear();
	for (const auto &entry : m_store->entries()) {
		m_knownKeys.insert(entry.key);
	}
	m_importCollection = m_collection;
	m_expectedKey = expectedKey;
	m_managedDirectory = copy ? QDir(m_profile->getPath()).filePath("library-media") : QString();
	m_cancel = std::make_shared<std::atomic_bool>(false);
	m_importButton->setEnabled(false);
	m_findSource->setEnabled(false);
	m_previousPage->setEnabled(false);
	m_nextPage->setEnabled(false);
	m_progress = new QProgressDialog(tr("Finding pictures…"), tr("Cancel"), 0, 0, this);
	m_progress->setWindowTitle(tr("Import pictures"));
	m_progress->setWindowModality(Qt::NonModal);
	m_progress->setMinimumDuration(0);
	m_progress->setAutoClose(false);
	connect(m_progress, &QProgressDialog::canceled, this, [cancel = m_cancel]() { cancel->store(true); });
	m_progress->show();
	auto *watcher = new QFutureWatcher<QStringList>(this);
	connect(watcher, &QFutureWatcher<QStringList>::finished, this, [this, watcher]() {
		m_importFiles = watcher->result();
		watcher->deleteLater();
		m_progress->setRange(0, qMax(1, int(m_importFiles.size())));
		importNext();
	});
	watcher->setFuture(QtConcurrent::run([paths, cancel = m_cancel]() { return LibraryImporter::imageFiles(paths, cancel); }));
}

void LibraryTab::importNext()
{
	if (m_cancel->load() || m_importIndex >= m_importFiles.size()) {
		finishImport(); return;
	}
	const QString path = m_importFiles[m_importIndex];
	m_progress->setLabelText(tr("%1 of %2 · %3").arg(m_importIndex + 1).arg(m_importFiles.size()).arg(QFileInfo(path).fileName()));
	m_progress->setValue(m_importIndex);
	auto *watcher = new QFutureWatcher<LibraryImportData>(this);
	connect(watcher, &QFutureWatcher<LibraryImportData>::finished, this, [this, watcher, path]() {
		const auto data = watcher->result();
		watcher->deleteLater();
		if (!m_cancel->load() || data.error.isEmpty()) {
			const QString key = data.error.isEmpty() ? m_store->saveLocalImage(data, m_importCollection, m_expectedKey) : QString();
			if (key.isEmpty()) {
				++m_failed;
				if (m_importErrors.size() < 50) {
					m_importErrors.append(QFileInfo(path).fileName() + ": " + (data.error.isEmpty() ? m_store->lastError() : data.error));
				}
			} else if (m_knownKeys.contains(key)) {
				++m_duplicates;
			} else {
				m_knownKeys.insert(key); ++m_added;
			}
		}
		++m_importIndex;
		importNext();
	});
	watcher->setFuture(QtConcurrent::run([path, directory = m_managedDirectory, cancel = m_cancel]() { return LibraryImporter::inspect(path, directory, cancel); }));
}

void LibraryTab::finishImport()
{
	const bool cancelled = m_cancel->load();
	m_progress->hide();
	m_progress->deleteLater();
	m_progress = nullptr;
	m_importing = false;
	m_importButton->setEnabled(m_store->isReady());
	reload();
	QString summary = tr("Import: %1 added · %2 duplicates · %3 failed").arg(m_added).arg(m_duplicates).arg(m_failed);
	if (cancelled) {
		summary += tr(" · Cancelled; completed pictures were kept.");
	} else if (m_importFiles.isEmpty()) {
		summary += tr(" · No supported pictures found.");
	}
	int noTags = 0, noSource = 0, errors = 0;
	for (const auto &entry : m_store->entries(m_importCollection)) {
		if (!entry.image.contains("local_import")) { continue; }
		if (entry.tags().isEmpty()) { ++noTags; }
		if (entry.image.value("website").toString().isEmpty()) { ++noSource; }
		if (!entry.metadataErrors().isEmpty()) { ++errors; }
	}
	summary += tr(". Local pictures in this scope: %1 need tags · %2 need source · %3 metadata errors. Use the Library sidebar to review them. Missing tags do not prevent likes or favorites.").arg(noTags).arg(noSource).arg(errors);
	m_hint->setText(summary);
	if (!m_importErrors.isEmpty()) {
		auto *report = new QDialog(this);
		report->setAttribute(Qt::WA_DeleteOnClose);
		report->setWindowTitle(tr("Import report"));
		auto *layout = new QVBoxLayout(report);
		auto *text = new QPlainTextEdit(summary + "\n\n" + m_importErrors.join('\n'), report);
		text->setReadOnly(true);
		layout->addWidget(text);
		report->resize(650, 350);
		report->show();
	}
	emit importFinished(m_added, m_duplicates, m_failed);
}
