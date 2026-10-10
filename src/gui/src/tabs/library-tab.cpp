#include "tabs/library-tab.h"
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QImageReader>
#include <QInputDialog>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QProgressDialog>
#include <QPushButton>
#include <QRandomGenerator>
#include <QScrollBar>
#include <QSettings>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QtConcurrent>
#include <functional>
#include <numeric>
#include "downloader/download-queue.h"
#include "downloader/image-downloader.h"
#include "image-library-actions.h"
#include "main-window.h"
#include "models/image.h"
#include "models/library-importer.h"
#include "models/profile.h"
#include "picture-actions.h"
#include "ui/toast.h"
#include "utils/image-fingerprint.h"
#include "viewer/library-image-dialog.h"
#include "viewer/library-source-dialog.h"
#include "viewer/viewer-window.h"


namespace
{
	QString muted(const QPalette &palette)
	{
		const QColor a = palette.color(QPalette::WindowText), b = palette.color(QPalette::Window);
		return QColor((a.red() * 3 + b.red() * 2) / 5, (a.green() * 3 + b.green() * 2) / 5, (a.blue() * 3 + b.blue() * 2) / 5).name();
	}

	QString entryTitle(const LibraryEntry &entry)
	{
		QString name = entry.image.value("name").toString();
		if (name.isEmpty()) {
			name = entry.image.value("data").toObject().value("title").toString();
		}
		if (name.isEmpty()) {
			name = LibraryTab::tr("Picture #%1").arg(entry.image.value("id").toString());
		}
		return name;
	}

	QString uniquePath(const QString &folder, const QString &name)
	{
		const QFileInfo info(name);
		QString candidate = QDir(folder).filePath(name);
		for (int i = 2; QFile::exists(candidate); ++i) {
			candidate = QDir(folder).filePath(QStringLiteral("%1 (%2).%3").arg(info.completeBaseName()).arg(i).arg(info.suffix()));
		}
		return candidate;
	}
}


LibraryTab::LibraryTab(Profile *profile, MainWindow *parent, DownloadQueue *downloadQueue)
	: QWidget(parent), m_profile(profile), m_mainWindow(parent), m_downloadQueue(downloadQueue), m_store(profile->library())
{
	setObjectName("libraryTab");
	setAcceptDrops(true);
	m_copyImports = profile->getSettings()->value("Library/copyImports", false).toBool();
	setWindowTitle(tr("Library"));
	setMaximumWidth(16777214); // Existing convention for permanent tabs.
	auto *layout = new QHBoxLayout(this);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(0);

	// Sidebar: smart views and collections.
	auto *sidebar = new QWidget(this);
	sidebar->setObjectName("librarySidebarPanel");
	sidebar->setFixedWidth(230);
	auto *sideLayout = new QVBoxLayout(sidebar);
	sideLayout->setContentsMargins(16, 18, 10, 14);
	auto *heading = new QLabel(tr("Library"), sidebar);
	heading->setObjectName("pageTitle");
	sideLayout->addWidget(heading);
	m_sidebar = new QTreeWidget(sidebar);
	m_sidebar->setObjectName("librarySidebar");
	m_sidebar->setHeaderHidden(true);
	m_sidebar->setRootIsDecorated(false);
	m_sidebar->setIndentation(10);
	m_sidebar->setIconSize(QSize(32, 32));
	m_sidebar->setContextMenuPolicy(Qt::CustomContextMenu);
	m_sidebar->setFrameShape(QFrame::NoFrame);
	sideLayout->addWidget(m_sidebar, 1);
	auto *create = new QPushButton(tr("+ New collection"), sidebar);
	create->setObjectName("libraryNewCollection");
	create->setEnabled(m_store->isReady());
	sideLayout->addWidget(create);
	m_manage = new QPushButton(tr("Manage collection…"), sidebar);
	m_manage->setObjectName("libraryManageCollection");
	sideLayout->addWidget(m_manage);
	layout->addWidget(sidebar);

	auto *content = new QWidget(this);
	auto *contentLayout = new QVBoxLayout(content);
	contentLayout->setContentsMargins(20, 18, 20, 10);
	contentLayout->setSpacing(10);
	layout->addWidget(content, 1);

	// Title row
	auto *titleRow = new QHBoxLayout();
	auto *titles = new QVBoxLayout();
	titles->setSpacing(2);
	m_title = new QLabel(content);
	m_title->setObjectName("libraryTitle");
	m_title->setTextFormat(Qt::PlainText);
	m_count = new QLabel(content);
	m_count->setObjectName("libraryCount");
	titles->addWidget(m_title);
	titles->addWidget(m_count);
	titleRow->addLayout(titles, 1);
	m_selectAll = new QPushButton(tr("Select all"), content);
	m_selectAll->setObjectName("librarySelectAll");
	m_selectAll->setToolTip(tr("Select every picture in this view (Ctrl+A)"));
	m_downloadAll = new QPushButton(tr("⬇  Download all"), content);
	m_downloadAll->setObjectName("libraryDownloadAll");
	m_downloadAll->setToolTip(tr("Download the original of every picture in this view to your download folder"));
	m_importButton = new QPushButton(tr("Import…"), content);
	m_importButton->setObjectName("libraryImport");
	m_importButton->setEnabled(m_store->isReady());
	m_importButton->setToolTip(tr("Add pictures from your PC, or drop them anywhere on Library."));
	for (auto *button : {m_selectAll, m_downloadAll, m_importButton}) {
		titleRow->addWidget(button, 0, Qt::AlignVCenter);
	}
	contentLayout->addLayout(titleRow);

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
		for (const auto &key : m_grid->selectedKeys()) {
			for (const auto &path : m_entries.value(key).localPaths) {
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

	// Filter row
	auto *filters = new QHBoxLayout();
	m_search = new QLineEdit(content);
	m_search->setObjectName("librarySearch");
	m_search->setPlaceholderText(tr("Search titles, tags, sources and notes"));
	m_search->setClearButtonEnabled(true);
	m_filter = new QComboBox(content);
	m_filter->setObjectName("libraryFilter");
	m_filter->addItems({ tr("All pictures"), tr("♥ Liked"), tr("★ Favorites") });
	m_sort = new QComboBox(content);
	m_sort->setObjectName("librarySort");
	m_sort->addItems({ tr("Newest first"), tr("Oldest first"), tr("Shuffle") });
	auto *density = new QComboBox(content);
	density->setObjectName("libraryDensity");
	density->setAccessibleName(tr("Picture size"));
	density->addItems({ tr("Compact"), tr("Comfortable"), tr("Large") });
	density->setCurrentIndex(qBound(0, profile->getSettings()->value("Gallery/density", 1).toInt(), 2));
	filters->addWidget(m_search, 1);
	filters->addWidget(m_filter);
	filters->addWidget(m_sort);
	filters->addWidget(density);
	contentLayout->addLayout(filters);

	m_hint = new QLabel(content);
	m_hint->setObjectName("libraryHint");
	m_hint->setTextFormat(Qt::PlainText);
	m_hint->setWordWrap(true);
	contentLayout->addWidget(m_hint);

	// Selection bar
	m_selectionBar = new QFrame(content);
	m_selectionBar->setObjectName("librarySelectionBar");
	auto *barLayout = new QHBoxLayout(m_selectionBar);
	barLayout->setContentsMargins(12, 6, 8, 6);
	m_selectionCount = new QLabel(m_selectionBar);
	m_selectionCount->setObjectName("librarySelectionCount");
	barLayout->addWidget(m_selectionCount);
	m_actions = new ImageLibraryActions(profile, {}, m_selectionBar);
	barLayout->addWidget(m_actions);
	auto *downloadSelected = new QPushButton(tr("⬇ Download"), m_selectionBar);
	downloadSelected->setObjectName("libraryDownloadSelected");
	auto *saveSelected = new QPushButton(tr("Save to folder…"), m_selectionBar);
	saveSelected->setObjectName("librarySaveToFolder");
	saveSelected->setToolTip(tr("Copy files already on disk, or download the rest, into a folder you choose"));
	m_findSource = new QPushButton(tr("Find source…"), m_selectionBar);
	m_findSource->setObjectName("libraryFindSource");
	m_findSource->setToolTip(tr("Select one picture. Search an exact MD5 on a website, or compare source pictures already saved in Library."));
	auto *more = new QPushButton(tr("More…"), m_selectionBar);
	more->setObjectName("libraryMore");
	auto *remove = new QPushButton(tr("Remove"), m_selectionBar);
	remove->setObjectName("libraryRemove");
	auto *clear = new QPushButton(tr("✕"), m_selectionBar);
	clear->setObjectName("libraryClearSelection");
	clear->setToolTip(tr("Clear selection (Esc)"));
	for (auto *button : {downloadSelected, saveSelected, m_findSource, more, remove}) {
		barLayout->addWidget(button);
	}
	barLayout->addStretch();
	barLayout->addWidget(clear);
	m_selectionBar->hide();
	contentLayout->addWidget(m_selectionBar);

	m_stack = new QStackedWidget(content);
	m_grid = new ImageGridView(m_stack);
	m_grid->setObjectName("libraryGrid");
	m_grid->setActions({ImageGridView::Like, ImageGridView::Favorite, ImageGridView::Download});
	m_grid->setDensity(density->currentIndex());
	m_grid->setAccessibleName(tr("Library pictures"));
	m_stack->addWidget(m_grid);
	m_empty = new QLabel(m_stack);
	m_empty->setObjectName("libraryEmpty");
	m_empty->setAlignment(Qt::AlignCenter);
	m_empty->setWordWrap(true);
	m_empty->setMargin(32);
	m_stack->addWidget(m_empty);
	contentLayout->addWidget(m_stack, 1);

	const QPalette colors = palette();
	const QColor accent = colors.color(QPalette::Highlight);
	setStyleSheet(QStringLiteral(
		"#pageTitle { font-size: 18pt; font-weight: 700; }"
		"#libraryTitle { font-size: 22pt; font-weight: 700; }"
		"#libraryCount, #libraryHint, #libraryEmpty { color: %1; }"
		"#libraryEmpty { font-size: 12pt; }"
		"#librarySidebarPanel { border-right: 1px solid rgba(127, 127, 127, 40); }"
		"#librarySidebar { border: 0; background: transparent; }"
		"#librarySidebar::item { padding: 7px 6px; border-radius: 8px; }"
		"#librarySearch { padding: 7px 10px; border-radius: 10px; }"
		"#librarySelectAll, #libraryDownloadAll, #libraryImport { border-radius: 15px; padding: 6px 14px; }"
		"#librarySelectionBar { border-radius: 12px; background: rgba(%2, %3, %4, 34); border: 1px solid rgba(%2, %3, %4, 90); }"
		"#librarySelectionBar QPushButton { border-radius: 12px; padding: 4px 10px; }"
		"#librarySelectionCount { font-weight: 600; padding-right: 6px; background: transparent; }"
	).arg(muted(colors)).arg(accent.red()).arg(accent.green()).arg(accent.blue()));

	connect(create, &QPushButton::clicked, this, &LibraryTab::newCollection);
	connect(m_manage, &QPushButton::clicked, this, [this]() {
		if (auto *item = m_sidebar->currentItem()) {
			m_sidebar->scrollToItem(item);
			collectionMenu(m_sidebar->visualItemRect(item).center());
		}
	});
	connect(m_selectAll, &QPushButton::clicked, this, [this]() { m_grid->selectAll(); m_grid->setFocus(); });
	connect(m_downloadAll, &QPushButton::clicked, this, [this]() {
		if (m_viewKeys.size() > 50 && QMessageBox::question(this, tr("Download all"), tr("Download %1 pictures to your download folder?").arg(m_viewKeys.size())) != QMessageBox::Yes) {
			return;
		}
		download(m_viewKeys);
	});
	connect(downloadSelected, &QPushButton::clicked, this, [this]() { download(m_grid->selectedKeys()); });
	connect(saveSelected, &QPushButton::clicked, this, [this]() { saveToFolder(m_grid->selectedKeys()); });
	connect(m_findSource, &QPushButton::clicked, this, [this]() {
		if (m_grid->selectedKeys().size() == 1) { findSource(m_grid->selectedKeys().first()); }
	});
	connect(more, &QPushButton::clicked, this, [this, more]() { imageMenu(m_grid->selectedKeys(), more->mapToGlobal(QPoint(0, more->height()))); });
	connect(remove, &QPushButton::clicked, this, [this]() { removeFromLibrary(m_grid->selectedKeys()); });
	connect(clear, &QPushButton::clicked, m_grid, &QAbstractItemView::clearSelection);
	connect(density, &QComboBox::currentIndexChanged, this, [this](int value) {
		m_profile->getSettings()->setValue("Gallery/density", value);
		m_grid->setDensity(value);
	});
	connect(m_store, &LibraryStore::imageChanged, this, &LibraryTab::scheduleReload);
	connect(m_store, &LibraryStore::collectionsChanged, this, &LibraryTab::scheduleReload);
	m_searchTimer = new QTimer(this);
	m_searchTimer->setSingleShot(true);
	m_searchTimer->setInterval(150);
	connect(m_searchTimer, &QTimer::timeout, this, &LibraryTab::scheduleReload);
	connect(m_search, &QLineEdit::textChanged, this, [this]() { m_searchTimer->start(); });
	connect(m_filter, &QComboBox::currentIndexChanged, this, &LibraryTab::scheduleReload);
	connect(m_sort, &QComboBox::currentIndexChanged, this, [this]() {
		m_sort->setProperty("seed", QRandomGenerator::global()->generate());
		scheduleReload();
	});
	connect(m_sidebar, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem *item) {
		if (item != nullptr && item->data(0, Qt::UserRole).isValid()) {
			m_collection = item->data(0, Qt::UserRole).toLongLong();
			m_smartFilter = item->data(0, Qt::UserRole + 1).toInt();
			const QSignalBlocker blocker(m_filter);
			m_filter->setCurrentIndex(0);
			m_grid->clearSelection();
			m_grid->scrollToTop();
			scheduleReload();
		}
	});
	connect(m_sidebar, &QTreeWidget::customContextMenuRequested, this, &LibraryTab::collectionMenu);
	connect(m_grid->selectionModel(), &QItemSelectionModel::selectionChanged, this, &LibraryTab::updateSelection);
	connect(m_grid, &ImageGridView::openRequested, this, &LibraryTab::openImage);
	connect(m_grid, &ImageGridView::actionTriggered, this, &LibraryTab::gridAction);
	connect(m_grid, &ImageGridView::contextMenuRequested, this, &LibraryTab::imageMenu);
	reload();
}

void LibraryTab::showEvent(QShowEvent *event)
{
	QWidget::showEvent(event);
	const int density = qBound(0, m_profile->getSettings()->value("Gallery/density", 1).toInt(), 2);
	if (auto *control = findChild<QComboBox*>("libraryDensity")) {
		control->setCurrentIndex(density);
	}
}

void LibraryTab::showView(qint64 collection, int smartFilter)
{
	m_collection = collection;
	m_smartFilter = collection > 0 ? 0 : qBound(0, smartFilter, int(Duplicates));
	m_search->clear();
	m_filter->setCurrentIndex(0);
	m_grid->clearSelection();
	reload();
	m_grid->scrollToTop();
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

QStringList LibraryTab::duplicateKeys(const QList<LibraryEntry> &entries) const
{
	// Same file hash, or the same picture by appearance (strict, so edits stay apart).
	QList<ImageFingerprint> prints;
	QStringList md5s;
	for (const auto &entry : entries) {
		prints.append(ImageFingerprint::fromImage(QImage::fromData(entry.thumbnail)));
		md5s.append(entry.image.value("md5").toString().toLower());
	}
	QVector<int> group(entries.size());
	std::iota(group.begin(), group.end(), 0);
	std::function<int(int)> root = [&group, &root](int index) { return group[index] == index ? index : group[index] = root(group[index]); };
	for (int i = 0; i < entries.size(); ++i) {
		for (int j = i + 1; j < entries.size(); ++j) {
			if ((!md5s[i].isEmpty() && md5s[i] == md5s[j]) || prints[i].sameImage(prints[j])) {
				group[root(j)] = root(i);
			}
		}
	}
	QHash<int, QStringList> clusters;
	QList<int> order;
	for (int i = 0; i < entries.size(); ++i) {
		const int cluster = root(i);
		if (!clusters.contains(cluster)) {
			order.append(cluster);
		}
		clusters[cluster].append(entries[i].key);
	}
	QStringList result;
	for (const int cluster : order) {
		if (clusters[cluster].size() > 1) {
			result.append(clusters[cluster]);
		}
	}
	return result;
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
	const QStringList selected = m_grid->selectedKeys();
	const QSignalBlocker sidebarBlock(m_sidebar);
	m_sidebar->clear();
	const QStringList smartNames { tr("All pictures"), tr("Unsorted"), tr("♥ Liked"), tr("★ Favorites"), tr("Recently saved"), tr("Needs tags"), tr("Needs source"), tr("Metadata errors"), tr("Possible duplicates") };
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
			item->setIcon(0, QIcon(cover.scaled(32, 32, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation).copy(0, 0, 32, 32)));
		}
		if (collection.id == m_collection) {
			m_sidebar->setCurrentItem(item);
		}
	}
	m_title->setText(title);
	m_manage->setEnabled(m_store->isReady() && m_collection > 0);

	const QString search = m_search->text().trimmed();
	const int preference = m_filter->currentIndex();
	auto entries = m_store->entries(m_collection);
	QSet<QString> duplicates;
	if (m_collection == 0 && m_smartFilter == Duplicates) {
		const auto keys = duplicateKeys(entries);
		duplicates = QSet<QString>(keys.begin(), keys.end());
		QHash<QString, int> order;
		for (int i = 0; i < keys.size(); ++i) {
			order.insert(keys[i], i);
		}
		std::stable_sort(entries.begin(), entries.end(), [&order](const LibraryEntry &left, const LibraryEntry &right) { return order.value(left.key, INT_MAX) < order.value(right.key, INT_MAX); });
	} else if (m_sort->currentIndex() == 1) {
		std::reverse(entries.begin(), entries.end());
	} else if (m_sort->currentIndex() == 2) {
		QRandomGenerator rng(m_sort->property("seed").toUInt());
		std::shuffle(entries.begin(), entries.end(), rng);
	}
	m_entries.clear();
	m_viewKeys.clear();
	QList<ImageGridItem> items;
	// shortcut: one metadata pass per refresh; query pages if catalogs exceed tens of thousands of pictures.
	for (const auto &entry : entries) {
		const bool smart = m_collection == 0;
		if ((smart && m_smartFilter == Unsorted && entry.collectionCount > 0) || (smart && m_smartFilter == Liked && !entry.liked) || (smart && m_smartFilter == Favorites && !entry.favorite)
			|| (preference == 1 && !entry.liked) || (preference == 2 && !entry.favorite)
			|| (smart && m_smartFilter == RecentlySaved && QDateTime::fromString(entry.savedAt, Qt::ISODateWithMs) < QDateTime::currentDateTimeUtc().addDays(-7))
			|| (smart && ((m_smartFilter == NeedsTags && !entry.tags().isEmpty()) || (m_smartFilter == NeedsSource && !entry.image.value("website").toString().isEmpty()) || (m_smartFilter == MetadataErrors && entry.metadataErrors().isEmpty())))
			|| (smart && m_smartFilter == Duplicates && !duplicates.contains(entry.key))) {
			continue;
		}
		if (!search.isEmpty()) {
			const QString searchable = QString::fromUtf8(QJsonDocument(entry.image).toJson(QJsonDocument::Compact)) + " " + entry.notes;
			bool matches = true;
			for (const auto &word : search.split(' ', Qt::SkipEmptyParts)) {
				matches = matches && searchable.contains(word, Qt::CaseInsensitive);
			}
			if (!matches) {
				continue;
			}
		}
		m_entries.insert(entry.key, entry);
		m_viewKeys.append(entry.key);
		const QString name = entryTitle(entry);
		QString source = entry.image.value("website").toString();
		if (source.isEmpty()) {
			source = tr("Local file · source unlinked");
		}
		QStringList details { name, source, entry.tags().isEmpty() ? tr("Needs tags") : tr("%n tag(s)", "", entry.tags().size()) };
		if (!entry.metadataErrors().isEmpty()) {
			details.append(tr("Metadata error: %1").arg(entry.metadataErrors().join("; ")));
		}
		if (!entry.notes.isEmpty()) {
			details.append(tr("Note: %1").arg(entry.notes));
		}
		ImageGridItem item;
		item.key = entry.key;
		item.encoded = entry.thumbnail;
		item.title = name;
		item.tooltip = details.join('\n');
		item.liked = entry.liked;
		item.favorite = entry.favorite;
		item.badge = entry.image.value("website").toString().isEmpty() ? tr("Local") : QString();
		items.append(item);
	}

	auto *model = m_grid->gridModel();
	if (model->keys() == m_viewKeys) {
		// Same pictures in the same order: update in place so scrolling and selection stay put.
		for (const auto &item : items) {
			model->setRating(item.key, item.liked, item.favorite);
		}
	} else {
		const int scroll = m_grid->verticalScrollBar()->value();
		model->setItems(items);
		for (const auto &key : selected) {
			const int row = model->row(key);
			if (row >= 0) {
				m_grid->selectionModel()->select(model->index(row), QItemSelectionModel::Select);
			}
		}
		m_grid->doItemsLayout();
		m_grid->verticalScrollBar()->setValue(scroll);
	}

	QString hint;
	if (m_collection > 0) {
		hint = tr("Likes, favorites and notes here belong to this collection. A picture can be in several collections.");
	} else if (m_smartFilter == NeedsTags) {
		hint = tr("These pictures have no tags yet. Likes and favorites still work. Select one and use Find source, or recheck metadata from Import.");
	} else if (m_smartFilter == NeedsSource) {
		hint = tr("No website post is linked yet. Find source offers exact MD5 lookup and similar pictures already in Library.");
	} else if (m_smartFilter == MetadataErrors) {
		hint = tr("Imported, but a metadata reader reported an error. Open the picture for details, then recheck metadata.");
	} else if (m_smartFilter == Duplicates) {
		hint = tr("Same file or the same picture from different sources, shown next to each other. Edits and variants are kept apart.");
	}
	m_hint->setText(hint);
	m_hint->setVisible(!hint.isEmpty());
	m_selectAll->setEnabled(!m_viewKeys.isEmpty());
	m_downloadAll->setEnabled(!m_viewKeys.isEmpty());

	if (!m_store->isReady()) {
		m_empty->setText(m_store->lastError());
	} else if (!search.isEmpty() || preference > 0) {
		m_empty->setText(tr("No pictures match these filters."));
	} else if (m_collection > 0) {
		m_empty->setText(tr("This collection is empty.\nAdd pictures from Discover, search results, the viewer or Library."));
	} else if (m_smartFilter == Duplicates) {
		m_empty->setText(tr("No duplicates found. Nice and tidy."));
	} else {
		m_empty->setText(tr("Keep the pictures you want to find again.\n♥ Like or ★ Favorite anything in Discover, search results or the viewer — or drop files here to import them."));
	}
	m_stack->setCurrentWidget(m_viewKeys.isEmpty() ? static_cast<QWidget*>(m_empty) : static_cast<QWidget*>(m_grid));
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
	const QStringList keys = m_grid->selectedKeys();
	// Library entries already exist; actions need only their keys.
	m_actions->setSelection({}, keys, m_collection);
	m_selectionBar->setVisible(!keys.isEmpty());
	m_findSource->setEnabled(keys.size() == 1 && !m_importing);
	m_selectionCount->setText(tr("%n selected", "", keys.size()));
	const int total = int(m_viewKeys.size());
	m_count->setText(total == 1 ? tr("1 picture") : tr("%1 pictures").arg(total));
}

void LibraryTab::gridAction(ImageGridView::Action action, const QStringList &keys)
{
	if (action == ImageGridView::Download) {
		download(keys);
		return;
	}
	if (action == ImageGridView::Like || action == ImageGridView::Favorite) {
		const bool favorite = action == ImageGridView::Favorite;
		bool on = false;
		for (const auto &key : keys) {
			bool ok = false;
			on = PictureActions::toggleRating(m_store, key, m_collection, favorite, &ok);
			if (!ok) {
				Toast::show(this, m_store->lastError(), 3500);
				return;
			}
		}
		Toast::show(this, !on ? tr("Rating removed") : favorite ? tr("★ Favorited") : tr("♥ Liked"), 1600);
	}
}

void LibraryTab::download(const QStringList &keys)
{
	QList<QSharedPointer<Image>> images;
	int local = 0;
	for (const auto &key : keys) {
		const auto image = restoreImage(m_entries.contains(key) ? m_entries.value(key) : m_store->entry(key));
		if (image) {
			images.append(image);
		} else {
			++local;
		}
	}
	if (images.isEmpty()) {
		Toast::show(this, local > 0 ? tr("These pictures are local files. Use Save to folder to copy them.") : tr("Nothing to download."), 3200);
		return;
	}
	PictureActions::download(m_profile, m_downloadQueue, images, this);
}

void LibraryTab::saveToFolder(const QStringList &keys)
{
	if (keys.isEmpty()) {
		return;
	}
	QSettings *settings = m_profile->getSettings();
	const QString folder = QFileDialog::getExistingDirectory(this, tr("Save %n picture(s) to…", "", keys.size()), settings->value("Library/lastExportDir").toString());
	if (folder.isEmpty()) {
		return;
	}
	settings->setValue("Library/lastExportDir", folder);
	int copied = 0, queued = 0, failed = 0;
	for (const auto &key : keys) {
		const auto entry = m_entries.contains(key) ? m_entries.value(key) : m_store->entry(key);
		const auto image = restoreImage(entry);
		QString existing;
		for (const QString &path : entry.localPaths) {
			if (QFile::exists(path)) {
				existing = path;
				break;
			}
		}
		if (existing.isEmpty() && image && !image->savePath().isEmpty() && QFile::exists(image->savePath())) {
			existing = image->savePath();
		}
		if (!existing.isEmpty()) {
			QFile::copy(existing, uniquePath(folder, QFileInfo(existing).fileName())) ? ++copied : ++failed;
		} else if (image && m_downloadQueue != nullptr) {
			auto *downloader = new ImageDownloader(m_profile, image, settings->value("Save/filename", "%md5%.%ext%").toString(), folder, 1, true, true, m_downloadQueue);
			m_downloadQueue->add(DownloadQueue::Manual, downloader);
			++queued;
		} else {
			++failed;
		}
	}
	QStringList parts;
	if (copied > 0) { parts.append(tr("%n copied", "", copied)); }
	if (queued > 0) { parts.append(tr("%n downloading", "", queued)); }
	if (failed > 0) { parts.append(tr("%n unavailable", "", failed)); }
	Toast::show(this, parts.join(" · "), 3200);
}

void LibraryTab::removeFromLibrary(const QStringList &keys)
{
	if (keys.isEmpty() || QMessageBox::question(this, tr("Remove from Library"), (keys.size() == 1 ? tr("Remove this picture from Library and all collections? Downloaded files stay on disk.") : tr("Remove %1 pictures from Library and all collections? Downloaded files stay on disk.").arg(keys.size()))) != QMessageBox::Yes) {
		return;
	}
	for (const auto &key : keys) {
		if (!m_store->removeImage(key)) {
			QMessageBox::warning(this, tr("Library"), m_store->lastError());
			break;
		}
	}
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
	openPicture(key, m_viewKeys, m_collection);
}

void LibraryTab::openPicture(const QString &key, const QStringList &keys, qint64 collection)
{
	if (!m_store->contains(key, collection)) { return; }
	const auto entry = m_store->entry(key, collection);
	auto image = restoreImage(entry);
	if (!entry.localPaths.isEmpty() || !image || !image->savePath().isEmpty()) {
		auto *viewer = new LibraryImageDialog(m_profile, keys, key, collection, this);
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
	for (const QString &otherKey : keys) {
		auto other = otherKey == key ? image : restoreImage(m_entries.contains(otherKey) ? m_entries.value(otherKey) : m_store->entry(otherKey, collection));
		if (other && !other->isGallery()) {
			images.append(other);
		}
	}
	auto *viewer = new ViewerWindow(images, image, image->parentSite(), m_profile, m_mainWindow, nullptr, collection);
	viewer->show();
}

void LibraryTab::imageMenu(const QStringList &selected, const QPoint &globalPosition)
{
	if (selected.isEmpty()) {
		return;
	}
	const QString key = selected.first();
	const auto entry = m_entries.value(key);
	QMenu menu(this);
	m_actions->addToMenu(&menu);
	if (selected.size() == 1) {
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
	menu.addSeparator();
	menu.addAction(tr("Download"), this, [this, selected]() { download(selected); });
	menu.addAction(tr("Save to folder…"), this, [this, selected]() { saveToFolder(selected); });
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
	menu.addAction(tr("Remove from Library…"), this, [this, selected]() { removeFromLibrary(selected); });
	menu.exec(globalPosition);
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
	m_hint->show();
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
