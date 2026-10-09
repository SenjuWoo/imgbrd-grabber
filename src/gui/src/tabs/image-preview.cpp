#include "tabs/image-preview.h"
#include <QApplication>
#include <QBuffer>
#include <QDir>
#include <QEvent>
#include <QEventLoop>
#include <QFile>
#include <QFileDialog>
#include <QImageReader>
#include <QLabel>
#include <QMenu>
#include <QMovie>
#include <QPainter>
#include <QRandomGenerator>
#include <QSettings>
#include <QTimer>
#include <QToolButton>
#include <QtMath>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>
#include "downloader/download-queue.h"
#include "downloader/image-downloader.h"
#include "functions.h"
#include "helpers.h"
#include "image-context-menu.h"
#include "image-library-actions.h"
#include "logger.h"
#include "models/image.h"
#include "models/profile.h"
#include "models/site.h"
#include "network/network-follow.h"
#include "network/network-reply.h"
#include "ui/QBouton.h"


namespace
{
	constexpr qint64 MaxPreviewBytes = 16 * 1024 * 1024;
	constexpr int MaxPreviewDimension = 8192;
	constexpr qint64 MaxPreviewPixels = 16 * 1024 * 1024;

	QSize previewBounds(QSettings *settings)
	{
		if (settings->contains("Gallery/density")) {
			const int density = qBound(0, settings->value("Gallery/density").toInt(), 2);
			const int size = density == 0 ? 128 : density == 1 ? 180 : 256;
			return { size, size };
		}
		const qreal upscale = settings->value("thumbnailUpscale", 1.0).toDouble();
		const int size = qIsFinite(upscale) ? qFloor(qBound(32.0, 150 * upscale, 512.0)) : 150;
		return { size, size };
	}
}

QMovie *ImagePreview::m_loadingMovie = nullptr;

ImagePreview::ImagePreview(QSharedPointer<Image> image, QWidget *container, Profile *profile, DownloadQueue *downloadQueue, MainWindow *mainWindow, QObject *parent)
	: QObject(parent), m_image(image), m_container(container), m_profile(profile), m_downloadQueue(downloadQueue), m_mainWindow(mainWindow)
{
	resetThumbnailUrls();
	m_counter = image->counter();
	m_borderSize = qBound(0, m_profile->getSettings()->value("borders", 3).toInt(), 16);
	container->setFixedSize(previewBounds(m_profile->getSettings()) + QSize(2 * m_borderSize, 2 * m_borderSize));

	auto *layout = new QVBoxLayout();
	layout->setContentsMargins(0, 0, 0, 0);
	container->setLayout(layout);

	container->setContextMenuPolicy(Qt::CustomContextMenu);
	connect(container, &QWidget::customContextMenuRequested, this, &ImagePreview::customContextMenuRequested);
}

ImagePreview::~ImagePreview()
{
	if (m_reply != nullptr) {
		disconnect(m_reply, nullptr, this, nullptr);
		m_reply->abort();
		m_reply->deleteLater();
		m_reply = nullptr;
	}

	// We don't own the button, but it will likely be deleted soon as well
	m_bouton = nullptr;
}


void ImagePreview::showLoadingMessage()
{
	if (m_loadingMovie == nullptr) {
		auto *loadingMovie = new QMovie(":/images/loading.gif");
		if (m_loadingMovie == nullptr) {
			m_loadingMovie = loadingMovie;
			m_loadingMovie->start();
		} else {
			loadingMovie->deleteLater();
		}
	}

	auto *loadingLabel = new QLabel();
	loadingLabel->setMovie(m_loadingMovie);
	loadingLabel->setAlignment(Qt::AlignHCenter | Qt::AlignVCenter);
	loadingLabel->setScaledContents(true);

	auto *layout = m_container->layout();
	layout->addWidget(loadingLabel);
}

void ImagePreview::resetThumbnailUrls()
{
	if (m_profile->getSettings()->value("thumbnailSmartSize", true).toBool()) {
		m_thumbnailUrl = m_image->mediaForSize(previewBounds(m_profile->getSettings()), true).url;
	} else {
		m_thumbnailUrl = m_image->url(Image::Size::Thumbnail);
	}
	m_fallbackUrls.clear();
	for (const auto size : { Image::Size::Thumbnail, Image::Size::Sample }) {
		const QUrl url = m_image->url(size);
		if (url.isValid() && !url.isEmpty() && url != m_thumbnailUrl && !m_fallbackUrls.contains(url)) {
			m_fallbackUrls.append(url);
		}
	}
	m_redirectsSeen.clear();
	m_redirectHops = 0;
}

void ImagePreview::load()
{
	if (m_aborted || !m_container) {
		return;
	}
	if (!m_image->previewImage().isNull()) {
		finishedLoading();
		return;
	}
	Site *site = m_image->parentSite();
	if (!site || !m_thumbnailUrl.isValid() || m_thumbnailUrl.isEmpty()) {
		failThumbnail(tr("No thumbnail URL is available."));
		return;
	}
	if (m_reply != nullptr) {
		disconnect(m_reply, nullptr, this, nullptr);
		m_reply->deleteLater();
	} else {
		showLoadingMessage();
	}
	m_reply = site->get(site->fixUrl(m_thumbnailUrl.toString()), Site::QueryType::Thumbnail, m_image->parentUrl(), "preview");
	connect(m_reply, &NetworkReply::finished, this, &ImagePreview::finishedLoadingPreview);
	connect(m_reply, &NetworkReply::downloadProgress, this, [this](qint64 received, qint64 total) {
		if (received > MaxPreviewBytes || total > MaxPreviewBytes) {
			failThumbnail(tr("Thumbnail exceeds the 16 MiB preview limit."));
		}
	});
	connect(m_reply, &NetworkReply::readyRead, this, [this]() {
		if (m_reply && m_reply->bytesAvailable() > MaxPreviewBytes) {
			failThumbnail(tr("Thumbnail exceeds the 16 MiB preview limit."));
		}
	});
}

void ImagePreview::failThumbnail(const QString &reason)
{
	m_previewError = reason;
	if (m_reply) {
		disconnect(m_reply, nullptr, this, nullptr);
		m_reply->abort();
	}
	if (!m_aborted && m_image->parentSite() && !m_fallbackUrls.isEmpty()) {
		m_thumbnailUrl = m_fallbackUrls.takeFirst();
		m_redirectsSeen.clear();
		m_redirectHops = 0;
		load();
		return;
	}
	finishedLoading();
}

void ImagePreview::abort()
{
	m_aborted = true;
	if (m_reply != nullptr && m_reply->isRunning()) {
		m_reply->abort();
	}
}

void ImagePreview::setChecked(bool checked)
{
	m_checked = checked;

	if (m_bouton != nullptr) {
		m_bouton->setChecked(checked);
	}
	updateActionsVisibility();
}

void ImagePreview::setDownloadProgress(qint64 v1, qint64 v2)
{
	if (m_bouton != nullptr) {
		m_bouton->setProgress(v1, v2);
	}
}


void ImagePreview::finishedLoadingPreview()
{
	if (m_aborted || !m_container || !m_reply) {
		return;
	}
	if (m_reply->error() == NetworkReply::NetworkError::OperationCanceledError) {
		return;
	}

	const QUrl target = m_reply->attribute(QNetworkRequest::RedirectionTargetAttribute).toUrl();
	if (!target.isEmpty()) {
		const QUrl redirection = m_reply->url().resolved(target);
		QString reason;
		if (NetworkFollow::takeRedirect(m_reply->url(), redirection, &m_redirectsSeen, &m_redirectHops, &reason) == NetworkFollow::Action::Stop) {
			failThumbnail(tr("Thumbnail redirect stopped: %1").arg(reason));
			return;
		}
		m_thumbnailUrl = redirection;
		load();
		return;
	}
	if (m_reply->error() != NetworkReply::NetworkError::NoError) {
		failThumbnail(tr("Thumbnail could not load: %1").arg(m_reply->errorString()));
		return;
	}
	if (m_reply->bytesAvailable() > MaxPreviewBytes) {
		failThumbnail(tr("Thumbnail exceeds the 16 MiB preview limit."));
		return;
	}
	QByteArray data = m_reply->readAll();
	if (data.size() > MaxPreviewBytes) {
		failThumbnail(tr("Thumbnail exceeds the 16 MiB preview limit."));
		return;
	}
	QBuffer buffer(&data);
	buffer.open(QIODevice::ReadOnly);
	QImageReader reader(&buffer);
	reader.setAutoTransform(true);
	const QSize size = reader.size();
	if (size.isEmpty() || size.width() > MaxPreviewDimension || size.height() > MaxPreviewDimension
		|| static_cast<qint64>(size.width()) * size.height() > MaxPreviewPixels) {
		failThumbnail(tr("Thumbnail has invalid or excessive image dimensions."));
		return;
	}
	const QImage pixels = reader.read();
	if (pixels.isNull()) {
		failThumbnail(tr("Thumbnail could not be decoded: %1").arg(reader.errorString()));
		return;
	}
	m_previewError.clear();
	m_image->setPreviewImage(QPixmap::fromImage(pixels));
	finishedLoading();
}

void ImagePreview::finishedLoading()
{
	if (m_aborted || !m_container) {
		return;
	}
	auto *layout = m_container->layout();
	clearLayout(layout);
	delete m_actions.data();
	m_actions = nullptr;

	QSettings *settings = m_profile->getSettings();
	const bool resizeInsteadOfCropping = settings->value("resizeInsteadOfCropping", true).toBool();
	const bool resultsScrollArea = settings->value("resultsScrollArea", true).toBool();
	auto *button = new QBouton(0, resizeInsteadOfCropping, resultsScrollArea, m_borderSize, m_image->color(), m_container);
	button->setObjectName("imagePreviewButton");
	button->setCheckable(true);
	button->setFlat(true);
	button->setChecked(m_checked);
	button->setInvertToggle(settings->value("invertToggle", false).toBool());
	QString tooltip = m_image->tooltip();
	if (!m_previewError.isEmpty()) {
		tooltip += QStringLiteral("<br><br>%1").arg(m_previewError.toHtmlEscaped());
	}
	button->setToolTip(tooltip);
	button->setAccessibleName(m_image->name().isEmpty() ? tr("Image preview") : m_image->name());
	button->setAccessibleDescription(m_previewError.isEmpty() ? tr("Enter opens the image; Space selects it. Selected images show Library actions.") : m_previewError);

	m_displayImage = m_image->previewImage();
	if (m_displayImage.isNull()) {
		m_displayImage = QPixmap(m_image->hasTag(QStringLiteral("flash")) ? ":/images/flash.png" : ":/images/noimage.png");
	} else if (m_image->isVideo() && settings->value("Interface/previewVideoIndicator", false).toBool()) {
		static const QPixmap overlay(":/images/thumbnail-video-overlay.png");
		const int overlaySize = qMin(qMin(overlay.width(), m_displayImage.width()), qMin(overlay.height(), m_displayImage.height()));
		QPainter painter(&m_displayImage);
		painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
		painter.drawPixmap(qMax(0, (m_displayImage.width() - overlaySize) / 2), qMax(0, (m_displayImage.height() - overlaySize) / 2), overlaySize, overlaySize, overlay);
	}
	if (!m_counter.isEmpty()) {
		button->setCounter(m_counter);
	}
	connect(button, SIGNAL(appui(int)), this, SIGNAL(clicked()));
	connect(button, SIGNAL(toggled(int, bool, bool)), this, SLOT(toggledWithId(int, bool, bool)));
	layout->addWidget(button);
	m_bouton = button;
	button->installEventFilter(this);

	m_actions = new ImageLibraryActions(m_profile, m_image, button, 0, true);
	QPalette palette = m_actions->palette();
	QColor background = palette.color(QPalette::Window);
	background.setAlpha(230);
	palette.setColor(QPalette::Window, background);
	m_actions->setPalette(palette);
	m_actions->setAutoFillBackground(true);
	for (auto *actionButton : m_actions->findChildren<QToolButton*>()) {
		actionButton->installEventFilter(this);
	}
	refreshDensity();
	updateActionsVisibility();
	emit finished();
}

void ImagePreview::refreshDensity()
{
	if (!m_container) {
		return;
	}
	const QSize bounds = previewBounds(m_profile->getSettings());
	const QSize tileSize = bounds + QSize(2 * m_borderSize, 2 * m_borderSize);
	m_container->setFixedSize(tileSize);
	if (m_bouton) {
		m_bouton->scale(m_displayImage, bounds);
		m_bouton->setFixedSize(tileSize);
	}
	if (m_actions) {
		const int height = qMin(tileSize.height(), m_actions->sizeHint().height());
		m_actions->setGeometry(0, tileSize.height() - height, tileSize.width(), height);
		m_actions->raise();
	}
}

void ImagePreview::updateActionsVisibility()
{
	if (m_actions) {
		QWidget *focus = QApplication::focusWidget();
		m_actions->setVisible(m_checked || (focus && (focus == m_bouton || m_actions->isAncestorOf(focus))));
	}
}

bool ImagePreview::eventFilter(QObject *object, QEvent *event)
{
	if (event->type() == QEvent::FocusIn || event->type() == QEvent::FocusOut) {
		QTimer::singleShot(0, this, [this]() { updateActionsVisibility(); });
	}
	return QObject::eventFilter(object, event);
}

void ImagePreview::toggledWithId(int id, bool toggle, bool range)
{
	Q_UNUSED(id)

	setChecked(toggle);
	emit toggled(toggle, range);
}


QStringList getImageAlreadyExists(const QSharedPointer<Image> &img, Profile *profile)
{
	QSettings *settings = profile->getSettings();
	const QString path = settings->value("Save/path").toString().replace("\\", "/");
	const QString fn = settings->value("Save/filename").toString();

	if (Filename(fn).needExactTags(img->parentSite(), settings) == 0) {
		QStringList ret;
		QStringList files = img->paths(fn, path, 0);
		for (const QString &file : files) {
			if (QFile(file).exists()) {
				ret.append(file);
			}
		}
		if (!ret.isEmpty()) {
			return ret;
		}
	}

	return profile->md5Exists(img->md5());
}

void ImagePreview::customContextMenuRequested()
{
	QMenu *menu = new ImageContextMenu(m_profile->getSettings(), m_image, m_mainWindow, m_container);
	QAction *first = menu->actions().first();
	if (!m_previewError.isEmpty()) {
		auto *retry = new QAction(tr("Retry thumbnail"), menu);
		connect(retry, &QAction::triggered, this, [this]() {
			resetThumbnailUrls();
			load();
		});
		menu->insertAction(first, retry);
	}

	// Save image
	QAction *actionSave;
	if (!getImageAlreadyExists(m_image, m_profile).isEmpty()) {
		actionSave = new QAction(QIcon(":/images/status/error.png"), tr("Delete"), menu);
	} else {
		actionSave = new QAction(QIcon(":/images/icons/save.png"), tr("Save"), menu);
	}
	connect(actionSave, &QAction::triggered, this, &ImagePreview::contextSaveImage);
	menu->insertAction(first, actionSave);

	// Save image as...
	QAction *actionSaveAs = new QAction(QIcon(":/images/icons/save-as.png"), tr("Save as..."), menu);
	connect(actionSaveAs, &QAction::triggered, this, &ImagePreview::contextSaveImageAs);
	menu->insertAction(first, actionSaveAs);

	// Custom elements
	if (m_customContextMenu != nullptr) {
		m_customContextMenu(menu, m_image);
	}

	menu->insertSeparator(first);

	menu->exec(QCursor::pos());
}

void ImagePreview::contextSaveImage()
{
	QStringList already = getImageAlreadyExists(m_image, m_profile);
	if (!already.isEmpty()) {
		m_image->remove(already);
	} else {
		QSettings *settings = m_profile->getSettings();
		const QString fn = settings->value("Save/filename").toString();
		const QString path = settings->value("Save/path").toString();

		auto *downloader = new ImageDownloader(m_profile, m_image, fn, path, 1, true, true, m_downloadQueue);
		connect(downloader, &ImageDownloader::downloadProgress, this, &ImagePreview::contextSaveImageProgress);
		m_downloadQueue->add(DownloadQueue::Manual, downloader);
	}
}

void ImagePreview::contextSaveImageAs()
{
	QSettings *settings = m_profile->getSettings();

	Filename format(settings->value("Save/filename").toString());
	QString tmpPath;

	// If we need detailed tags for the filename, we first load them
	const int needTags = format.needExactTags(m_image->parentSite(), settings);
	if (needTags == 2 || (needTags == 1 && m_image->hasUnknownTag())) {
		QEventLoop loop;
		m_image->loadDetails();
		connect(m_image.data(), &Image::finishedLoadingTags, &loop, &QEventLoop::quit);
		loop.exec();
	}

	// If the MD5 is required for the filename, we first download the image
	if (format.needTemporaryFile(m_image->tokens(m_profile))) {
		tmpPath = QDir::temp().absoluteFilePath("grabber-saveAs-" + QString::number(QRandomGenerator::global()->generate(), 16));

		QEventLoop loop;
		ImageDownloader downloader(m_profile, m_image, { tmpPath }, 1, true, true, this);
		connect(&downloader, &ImageDownloader::saved, &loop, &QEventLoop::quit);
		downloader.save();
		loop.exec();
	}

	const QStringList filenames = format.path(*m_image, m_profile);
	const QString filename = filenames.first().section(QDir::separator(), -1);
	const QString lastDir = settings->value("Viewer/lastDir").toString();

	QString path = QFileDialog::getSaveFileName(m_container, tr("Save image"), QDir::toNativeSeparators(lastDir + "/" + filename), "Images (*.png *.gif *.jpg *.jpeg)");
	if (!path.isEmpty()) {
		path = QDir::toNativeSeparators(path);
		settings->setValue("Viewer/lastDir", path.section(QDir::separator(), 0, -2));

		if (!tmpPath.isEmpty()) {
			QFile::rename(tmpPath, path);
		} else {
			auto *downloader = new ImageDownloader(m_profile, m_image, { path }, 1, true, true, this, true, false, Image::Size::Unknown, true, true);
			connect(downloader, &ImageDownloader::downloadProgress, this, &ImagePreview::contextSaveImageProgress);
			m_downloadQueue->add(DownloadQueue::Manual, downloader);
		}
	} else if (!tmpPath.isEmpty()) {
		QFile::remove(tmpPath);
	}
}

void ImagePreview::contextSaveImageProgress(const QSharedPointer<Image> &img, qint64 v1, qint64 v2)
{
	Q_UNUSED(img)
	setDownloadProgress(v1, v2);
}

void ImagePreview::setCustomContextMenu(std::function<void (QMenu *, const QSharedPointer<Image> &)> customContextMenu)
{
	m_customContextMenu = std::move(customContextMenu);
}
