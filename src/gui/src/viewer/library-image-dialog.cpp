#include "viewer/library-image-dialog.h"
#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QGraphicsPixmapItem>
#include <QGraphicsScene>
#include <QGraphicsView>
#include <QHBoxLayout>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QMessageBox>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QResizeEvent>
#include <QShortcut>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QSplitter>
#include <QTextDocument>
#include <QTabWidget>
#include <QUrl>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <functional>
#include "image-library-actions.h"
#include "models/library-store.h"
#include "models/profile.h"


namespace
{
	QString sourceUrl(const LibraryEntry &entry)
	{
		QStringList candidates { entry.image.value("page_url").toString() };
		for (const auto &value : entry.image.value("local_import").toObject().value("source_urls").toArray()) {
			candidates.append(value.toString());
		}
		for (const QString &candidate : candidates) {
			const QUrl url(candidate, QUrl::StrictMode);
			if (url.isValid() && !url.host().isEmpty() && url.userInfo().isEmpty()
				&& (url.scheme().toLower() == "https" || url.scheme().toLower() == "http")) {
				return url.toString();
			}
		}
		return {};
	}
}


LibraryImageDialog::LibraryImageDialog(Profile *profile, const QStringList &keys, const QString &key, qint64 collection, QWidget *parent)
	: QDialog(parent), m_profile(profile), m_store(profile->library()), m_keys(keys), m_collection(collection)
{
	setObjectName("libraryImageDialog");
	setAttribute(Qt::WA_DeleteOnClose);
	setWindowTitle(tr("Picture Library"));
	resize(1160, 760);
	m_keys.removeDuplicates();
	if (!key.isEmpty() && !m_keys.contains(key)) {
		m_keys.append(key);
	}
	m_index = qMax(0, m_keys.indexOf(key));
	auto *layout = new QVBoxLayout(this);
	auto *header = new QHBoxLayout;
	m_title = new QLabel(this);
	m_title->setObjectName("libraryImageTitle");
	m_title->setTextFormat(Qt::PlainText);
	m_title->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
	m_title->setStyleSheet("font-size: 16px; font-weight: 600;");
	header->addWidget(m_title, 1);
	m_position = new QLabel(this);
	header->addWidget(m_position);
	m_previous = new QPushButton(tr("Previous"), this);
	m_previous->setObjectName("libraryImagePrevious");
	m_next = new QPushButton(tr("Next"), this);
	m_next->setObjectName("libraryImageNext");
	header->addWidget(m_previous);
	header->addWidget(m_next);
	auto *fit = new QPushButton(tr("Fit"), this);
	auto *actual = new QPushButton(tr("Preview 1:1"), this);
	actual->setToolTip(tr("One display pixel per decoded preview pixel. The preview is limited to 4096 pixels; Open original opens the full file."));
	auto *out = new QPushButton(tr("−"), this);
	auto *in = new QPushButton(tr("+"), this);
	fit->setObjectName("libraryImageFit");
	for (auto *button : {fit, actual, out, in}) {
		header->addWidget(button);
	}
	layout->addLayout(header);
	m_actions = new ImageLibraryActions(profile, {}, this, collection);
	layout->addWidget(m_actions);
	auto *splitter = new QSplitter(this);
	m_view = new QGraphicsView(splitter);
	m_view->setObjectName("libraryImageView");
	m_view->setScene(new QGraphicsScene(m_view));
	m_view->setDragMode(QGraphicsView::ScrollHandDrag);
	m_view->setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
	m_view->setResizeAnchor(QGraphicsView::AnchorViewCenter);
	m_view->setRenderHints(QPainter::SmoothPixmapTransform);
	m_view->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	m_view->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	m_view->viewport()->installEventFilter(this);
	auto *inspector = new QWidget(splitter);
	inspector->setMinimumWidth(220);
	auto *details = new QVBoxLayout(inspector);
	details->setContentsMargins(8, 0, 0, 0);
	details->addWidget(new QLabel(tr("File and source information"), inspector));
	m_metadata = new QPlainTextEdit(inspector);
	m_metadata->setObjectName("libraryImageMetadata");
	m_metadata->setReadOnly(true);
	m_metadata->setLineWrapMode(QPlainTextEdit::WidgetWidth);
	auto *information = new QTabWidget(inspector);
	information->addTab(m_metadata, tr("Overview"));
	m_rawMetadata = new QPlainTextEdit(information);
	m_rawMetadata->setReadOnly(true);
	m_rawMetadata->setLineWrapMode(QPlainTextEdit::WidgetWidth);
	information->addTab(m_rawMetadata, tr("Raw metadata"));
	details->addWidget(information, 1);
	details->addWidget(new QLabel(tr("Notes for this scope"), inspector));
	m_notes = new QPlainTextEdit(inspector);
	m_notes->setObjectName("libraryImageNotes");
	m_notes->setMaximumHeight(120);
	details->addWidget(m_notes);
	m_saveNotes = new QPushButton(tr("Save notes"), inspector);
	m_saveNotes->setObjectName("libraryImageSaveNotes");
	details->addWidget(m_saveNotes);
	splitter->setStretchFactor(0, 1);
	splitter->setStretchFactor(1, 0);
	splitter->setSizes({820, 300});
	layout->addWidget(splitter, 1);
	m_status = new QLabel(this);
	m_status->setObjectName("libraryImageStatus");
	m_status->setTextFormat(Qt::PlainText);
	m_status->setWordWrap(true);
	layout->addWidget(m_status);
	auto *buttons = new QHBoxLayout;
	m_reveal = new QPushButton(tr("Reveal folder"), this);
	m_open = new QPushButton(tr("Open original"), this);
	m_source = new QPushButton(tr("Open source"), this);
	auto *locate = new QPushButton(tr("Locate file…"), this);
	auto *findSource = new QPushButton(tr("Find / link source…"), this);
	auto *toggleDetails = new QPushButton(tr("Details"), this);
	toggleDetails->setCheckable(true);
	toggleDetails->setChecked(true);
	m_open->setObjectName("libraryImageOpenOriginal");
	m_source->setObjectName("libraryImageOpenSource");
	locate->setObjectName("libraryImageLocate");
	findSource->setObjectName("libraryImageFindSource");
	for (auto *button : {m_reveal, m_open, m_source, locate, findSource, toggleDetails}) {
		buttons->addWidget(button);
	}
	buttons->addStretch();
	layout->addLayout(buttons);
	connect(m_previous, &QPushButton::clicked, this, [this]() { navigate(-1); });
	connect(m_next, &QPushButton::clicked, this, [this]() { navigate(1); });
	connect(fit, &QPushButton::clicked, this, &LibraryImageDialog::fitImage);
	connect(actual, &QPushButton::clicked, this, [this]() { m_fit = false; m_view->resetTransform(); });
	connect(out, &QPushButton::clicked, this, [this]() { zoomImage(1 / 1.2); });
	connect(in, &QPushButton::clicked, this, [this]() { zoomImage(1.2); });
	connect(toggleDetails, &QPushButton::toggled, inspector, &QWidget::setVisible);
	connect(m_saveNotes, &QPushButton::clicked, this, [this]() { saveNotes(); });
	connect(m_notes, &QPlainTextEdit::textChanged, this, [this]() { m_saveNotes->setEnabled(m_notes->document()->isModified()); });
	connect(m_reveal, &QPushButton::clicked, this, [this]() { QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(m_filePath).absolutePath())); });
	connect(m_open, &QPushButton::clicked, this, [this]() { QDesktopServices::openUrl(QUrl::fromLocalFile(m_filePath)); });
	connect(m_source, &QPushButton::clicked, this, [this]() { QDesktopServices::openUrl(QUrl(m_sourceUrl)); });
	connect(locate, &QPushButton::clicked, this, [this]() {
		if (!m_keys.isEmpty()) {
			emit locateRequested(m_keys.at(m_index));
		}
	});
	connect(findSource, &QPushButton::clicked, this, [this]() {
		if (!m_keys.isEmpty()) {
			emit sourceRequested(m_keys.at(m_index));
		}
	});
	auto shortcut = [this](const QKeySequence &sequence, const std::function<void()> &callback) {
		auto *action = new QShortcut(sequence, m_view);
		action->setContext(Qt::WidgetWithChildrenShortcut);
		connect(action, &QShortcut::activated, this, callback);
	};
	shortcut(QKeySequence(Qt::Key_Left), [this]() { navigate(-1); });
	shortcut(QKeySequence(Qt::Key_Right), [this]() { navigate(1); });
	shortcut(QKeySequence(Qt::Key_F), [this]() { fitImage(); });
	shortcut(QKeySequence(Qt::Key_Plus), [this]() { zoomImage(1.2); });
	shortcut(QKeySequence(Qt::Key_Minus), [this]() { zoomImage(1 / 1.2); });
	connect(m_store, &LibraryStore::imageChanged, this, [this](const QString &changed) {
		if (!m_keys.isEmpty() && (changed.isEmpty() || changed == m_keys.at(m_index))) {
			showCurrent();
		}
	});
	connect(m_store, &LibraryStore::collectionsChanged, this, &LibraryImageDialog::showCurrent);
	showCurrent();
	m_view->setFocus();
}


void LibraryImageDialog::showCurrent()
{
	if (!m_store || m_keys.isEmpty()) {
		return;
	}
	const QString key = m_keys.at(m_index);
	const bool available = m_store->contains(key, m_collection);
	LibraryEntry entry = m_store->entry(key, m_collection);
	if (entry.key.isEmpty() && m_lastEntry.key == key) {
		entry = m_lastEntry;
	}
	if (!entry.key.isEmpty()) {
		m_lastEntry = entry;
	}
	const bool navigated = property("libraryImageKey").toString() != key;
	setProperty("libraryImageKey", key);
	m_title->setText(entry.image.value("name").toString(tr("Picture")));
	m_position->setText(tr("%1 / %2").arg(m_index + 1).arg(m_keys.size()));
	m_previous->setEnabled(m_index > 0);
	m_next->setEnabled(m_index + 1 < m_keys.size());
	m_actions->setSelection({}, {key}, m_collection);
	m_actions->setEnabled(available);
	QStringList paths = entry.localPaths;
	if (paths.isEmpty()) {
		const QString md5 = entry.image.value("md5").toString();
		if (!md5.isEmpty()) {
			paths = m_profile->md5Exists(md5);
		}
	}
	QString existing;
	for (const QString &path : paths) {
		if (QFileInfo(path).isFile()) {
			existing = path;
			break;
		}
	}
	const QString desiredPath = existing.isEmpty() ? paths.value(0) : existing;
	const bool reloadImage = navigated || desiredPath != m_filePath || m_view->scene()->items().isEmpty();
	m_filePath = desiredPath;
	m_sourceUrl = sourceUrl(entry);
	m_open->setEnabled(!existing.isEmpty());
	m_source->setEnabled(!m_sourceUrl.isEmpty());
	m_source->setToolTip(m_sourceUrl);
	m_reveal->setEnabled(!m_filePath.isEmpty() && QFileInfo(QFileInfo(m_filePath).absolutePath()).isDir());
	const auto local = entry.image.value("local_import").toObject();
	const QStringList tags = entry.tags();
	QStringList origins, warnings;
	for (const auto &value : local.value("evidence").toArray()) {
		const auto evidence = value.toObject();
		const QString kind = evidence.value("kind").toString();
		const QMap<QString, QString> labels {{"embedded-text", tr("Embedded text")}, {"filename-md5", tr("MD5 filename")}, {"sidecar-json", tr("JSON sidecar")}, {"sidecar-xmp", tr("XMP sidecar")}, {"sidecar-txt", tr("Tag sidecar")}, {"download-origin", tr("Browser download information")}, {"exiftool", tr("Embedded metadata (ExifTool)")}};
		const QString label = labels.value(kind, kind);
		if (!origins.contains(label)) {
			origins.append(label);
		}
		if (!evidence.value("error").toString().isEmpty()) {
			warnings.append(label + ": " + evidence.value("error").toString());
		}
	}
	const QString dimensions = local.value("width").toInt() > 0 && local.value("height").toInt() > 0
		? tr("%1 × %2 pixels").arg(local.value("width").toInt()).arg(local.value("height").toInt()) : tr("Original dimensions not recorded");
	const QString sourceDescription = m_sourceUrl.isEmpty() ? tr("Not linked") : m_sourceUrl
		+ (entry.image.value("website").toString().isEmpty() ? "\n" + tr("From metadata; source not verified") : QString());
	QString overview = dimensions + "\n\n" + tr("Source") + "\n" + sourceDescription
		+ "\n\n" + tr("Tags") + "\n" + (tags.isEmpty() ? tr("Needs tags — none available in the saved metadata") : tags.join(", "))
		+ "\n\n" + tr("Known files") + "\n" + (paths.isEmpty() ? tr("No local file is linked.") : paths.join('\n'))
		+ "\n\n" + tr("Metadata recovered from") + "\n" + (origins.isEmpty() ? tr("No source metadata found") : origins.join('\n'));
	if (!local.value("preview_error").toString().isEmpty()) {
		overview += "\n\n" + tr("Preview") + "\n" + local.value("preview_error").toString();
	}
	if (!warnings.isEmpty()) {
		overview += "\n\n" + tr("Metadata read errors") + "\n" + warnings.join('\n');
	}
	if (local.value("extended_reader").toString() == "unavailable") {
		overview += "\n\n" + tr("Extended metadata reader") + "\n" + tr("ExifTool was unavailable during the last import. Basic embedded text and sidecars were checked. Install ExifTool beside Grabber or in PATH, then use Import pictures → Recheck metadata for EXIF/IPTC/XMP coverage.");
	}
	overview += "\n\n" + tr("Likes, favorites and collections work even without tags, and stay attached when you link a source. Recommendations are planned; no recommendation feed is running yet.");
	m_metadata->setPlainText(overview);
	m_rawMetadata->setPlainText(QString::fromUtf8(QJsonDocument(entry.image).toJson(QJsonDocument::Indented)));
	if ((available && (navigated || !m_notes->document()->isModified())) || (navigated && !available)) {
		const QSignalBlocker blocked(m_notes);
		m_notes->setPlainText(available ? entry.notes : QString());
		m_notes->document()->setModified(false);
	}
	m_notes->setEnabled(!entry.key.isEmpty());
	m_notes->setReadOnly(!available);
	m_saveNotes->setEnabled(m_notes->document()->isModified());
	const QString removed = available ? QString() : tr("This picture or collection was removed from this view. Ratings are disabled; edited notes remain available to copy.");
	if (!removed.isEmpty()) {
		m_status->setText(removed);
		m_status->show();
	}
	if (!reloadImage) {
		return;
	}
	QImage image;
	QString notice;
	if (!existing.isEmpty()) {
		QImageReader reader(existing);
		reader.setAutoTransform(true);
		const QSize size = reader.size();
		if (size.isValid() && qint64(size.width()) * size.height() <= 40000000 && size.width() <= 32768 && size.height() <= 32768) {
			if (size.width() > 4096 || size.height() > 4096) {
				reader.setScaledSize(size.scaled(QSize(4096, 4096), Qt::KeepAspectRatio));
			}
			image = reader.read();
		}
		if (image.isNull()) {
			notice = tr("The original could not be decoded. Showing the saved preview.");
		}
	} else {
		notice = tr("The local file is unavailable. Showing the saved preview; likes, favorites and notes remain available. Use Locate file to reconnect it.");
	}
	if (image.isNull()) {
		image.loadFromData(entry.thumbnail);
	}
	if (!image.isNull() && (image.width() > 4096 || image.height() > 4096)) {
		image = image.scaled(QSize(4096, 4096), Qt::KeepAspectRatio, Qt::SmoothTransformation);
	}
	m_view->scene()->clear();
	if (image.isNull()) {
		notice = !existing.isEmpty() && !local.value("preview_error").toString().isEmpty()
			? tr("This picture exceeds the preview limit. Use Open file to view the original externally. Likes, favorites, notes and source linking still work.")
			: tr("No local image or saved preview is available. Use Locate file to reconnect this picture.");
	} else {
		m_view->scene()->addPixmap(QPixmap::fromImage(image));
	}
	m_view->scene()->setSceneRect(m_view->scene()->itemsBoundingRect());
	m_status->setText(removed.isEmpty() ? notice : removed + (notice.isEmpty() ? QString() : "\n" + notice));
	m_status->setVisible(!m_status->text().isEmpty());
	fitImage();
}


void LibraryImageDialog::navigate(int direction)
{
	const int next = m_index + direction;
	if (next < 0 || next >= m_keys.size() || !saveNotes()) {
		return;
	}
	m_index = next;
	showCurrent();
}


bool LibraryImageDialog::saveNotes()
{
	if (!m_store || m_keys.isEmpty() || !m_notes->document()->isModified()) {
		return true;
	}
	if (!m_store->contains(m_keys.at(m_index), m_collection)) {
		QMessageBox message(QMessageBox::Warning, tr("Notes scope removed"), tr("This picture or collection was removed. The edited notes cannot be saved to their former scope."), QMessageBox::NoButton, this);
		auto *copy = message.addButton(tr("Copy notes and close"), QMessageBox::AcceptRole);
		message.addButton(QMessageBox::Cancel);
		message.setDefaultButton(QMessageBox::Cancel);
		message.setEscapeButton(QMessageBox::Cancel);
		message.exec();
		if (message.clickedButton() == copy) {
			QApplication::clipboard()->setText(m_notes->toPlainText());
			m_notes->document()->setModified(false);
			QDialog::reject();
		}
		return false;
	}
	if (!m_store->setNotes(m_keys.at(m_index), m_notes->toPlainText(), m_collection)) {
		QMessageBox::warning(this, tr("Picture Library"), m_store->lastError());
		return false;
	}
	m_notes->document()->setModified(false);
	m_saveNotes->setEnabled(false);
	return true;
}


void LibraryImageDialog::fitImage()
{
	m_fit = true;
	if (!m_view->scene()->sceneRect().isEmpty()) {
		m_view->fitInView(m_view->scene()->sceneRect(), Qt::KeepAspectRatio);
	}
}


void LibraryImageDialog::zoomImage(qreal factor)
{
	if (m_view->scene()->sceneRect().isEmpty()) {
		return;
	}
	m_fit = false;
	const qreal scale = m_view->transform().m11();
	const qreal next = qBound(qreal(0.05), scale * factor, qreal(32));
	if (scale > 0) {
		m_view->scale(next / scale, next / scale);
	}
}


bool LibraryImageDialog::eventFilter(QObject *watched, QEvent *event)
{
	if (watched == m_view->viewport() && event->type() == QEvent::Wheel) {
		auto *wheel = static_cast<QWheelEvent*>(event);
		zoomImage(wheel->angleDelta().y() > 0 ? 1.2 : 1 / 1.2);
		wheel->accept();
		return true;
	}
	return QDialog::eventFilter(watched, event);
}


void LibraryImageDialog::resizeEvent(QResizeEvent *event)
{
	QDialog::resizeEvent(event);
	if (m_fit && m_view) {
		fitImage();
	}
}


void LibraryImageDialog::showEvent(QShowEvent *event)
{
	QDialog::showEvent(event);
	if (m_fit) {
		fitImage();
	}
}


void LibraryImageDialog::closeEvent(QCloseEvent *event)
{
	if (!saveNotes()) {
		event->ignore();
		return;
	}
	QDialog::closeEvent(event);
}


void LibraryImageDialog::reject()
{
	if (saveNotes()) {
		QDialog::reject();
	}
}
