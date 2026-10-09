#include "tabs/search-tab.h"
#include <QCompleter>
#include <QEventLoop>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QSet>
#include <QRegularExpression>
#include <QShortcut>
#include <QtMath>
#include <algorithm>
#include "downloader/download-query-image.h"
#include "downloader/download-queue.h"
#include "downloader/image-downloader.h"
#include "filename/filename.h"
#include "functions.h"
#include "helpers.h"
#include "image-context-menu.h"
#include "logger.h"
#include "main-window.h"
#include "models/api/api.h"
#include "models/favorite.h"
#include "models/filtering/post-filter.h"
#include "models/page.h"
#include "models/profile.h"
#include "models/site.h"
#include "sources/sources-window.h"
#include "tabs/image-preview.h"
#include "ui/fixed-size-grid-layout.h"
#include "ui/QBouton.h"
#include "ui/text-edit.h"
#include "ui/verticalscrollarea.h"
#include "viewer/viewer-window.h"

#define FIXED_IMAGE_WIDTH 150


SearchTab::SearchTab(Profile *profile, DownloadQueue *downloadQueue, MainWindow *parent, QString screenName)
	: QWidget(parent), m_profile(profile), m_downloadQueue(downloadQueue), m_screenName(std::move(screenName)), m_sites(profile->getSites()), m_favorites(profile->getFavorites()), m_parent(parent), m_settings(profile->getSettings()), m_pageMax(-1), m_stop(true), m_from_history(false), m_history_cursor(0)
{
	setAttribute(Qt::WA_DeleteOnClose);

	// Checkboxes
	m_checkboxesSignalMapper = new QSignalMapper(this);
	connect(m_checkboxesSignalMapper, &QSignalMapper::mappedString, this, &SearchTab::toggleSource);

	// Modifiers
	for (auto it = m_sites.constBegin(); it != m_sites.constEnd(); ++it) {
		Site *site = it.value();
		if (site->getApis().isEmpty()) { continue; }
		const QStringList modifiers = site->getApis().first()->modifiers();
		m_completion.append(modifiers);
	}
	m_completion.removeDuplicates();

	// Auto-complete list
	m_completion.append(profile->getAutoComplete());

	setSelectedSources(m_settings);
}

void SearchTab::init()
{
	if (ui_spinImagesPerPage != nullptr) {
		ui_spinImagesPerPage->setToolTip(tr("Requested images per source. Each website may impose a lower limit; Next follows the source's actual pages."));
		auto *grid = qobject_cast<QGridLayout*>(ui_spinImagesPerPage->parentWidget()->layout());
		if (grid != nullptr) {
			m_density = new QComboBox(this);
			m_density->setObjectName("searchDensity");
			m_density->setAccessibleName(tr("Image density"));
			m_density->addItems({tr("Compact"), tr("Comfortable"), tr("Large")});
			if (!m_settings->contains("Gallery/density")) { m_settings->setValue("Gallery/density", 1); }
			m_density->setCurrentIndex(qBound(0, m_settings->value("Gallery/density", 1).toInt(), 2));
			grid->addWidget(m_density, 0, grid->columnCount());
			connect(m_density, &QComboBox::currentIndexChanged, this, [this](int density) {
				m_settings->setValue("Gallery/density", density);
				const int sizes[] = {128, 180, 256};
				const int bounds = sizes[density] + 2 * qBound(0, m_settings->value("borders", 3).toInt(), 16);
				for (auto *preview : m_boutons) { preview->refreshDensity(); }
				for (auto *layout : m_layouts) { layout->setFixedWidth(bounds); }
			});
		}
	}
	m_endlessLoadingEnabled = true;
	m_endlessLoadOffset = 0;
	const QString infinite = m_settings->value("infiniteScroll", "disabled").toString();

	// Always hide scroll button before results are loaded
	if (ui_buttonEndlessLoad != nullptr) {
		ui_buttonEndlessLoad->hide();
	}

	if (infinite == "scroll") {
		connect(ui_scrollAreaResults, &VerticalScrollArea::endOfScrollReached, this, &SearchTab::endlessLoad);
	}

	if (infinite != "disabled" && ui_checkMergeResults != nullptr) {
		connect(ui_checkMergeResults, &QCheckBox::toggled, this, &SearchTab::setMergeResultsMode);
	}

	// Fill post-filter explicitely if necessary
	if (m_settings->value("globalPostFilterExplicit", false).toBool()) {
		QString globalPostFilter = m_settings->value("globalPostFilter").toString();
		m_postFiltering->setText(globalPostFilter);
	}

	// Navigation keyboard shortcuts
	if (ui_buttonFirstPage != nullptr) {
		ui_buttonFirstPage->setShortcut(getKeySequence(m_settings, "Main/Shortcuts/keyFirstPage", Qt::CTRL | Qt::Key_Home));
	}
	if (ui_buttonPreviousPage != nullptr) {
		ui_buttonPreviousPage->setShortcut(getKeySequence(m_settings, "Main/Shortcuts/keyPreviousPage", Qt::CTRL | Qt::Key_Left));
	}
	if (ui_buttonNextPage != nullptr) {
		ui_buttonNextPage->setShortcut(getKeySequence(m_settings, "Main/Shortcuts/keyNextPage", Qt::CTRL | Qt::Key_Right));
	}
	if (ui_buttonLastPage != nullptr) {
		ui_buttonLastPage->setShortcut(getKeySequence(m_settings, "Main/Shortcuts/keyLastPage", Qt::CTRL | Qt::Key_End));
	}
}

void SearchTab::showEvent(QShowEvent *event)
{
	QWidget::showEvent(event);
	if (m_density != nullptr) { m_density->setCurrentIndex(qBound(0, m_settings->value("Gallery/density", 1).toInt(), 2)); }
}

SearchTab::~SearchTab()
{
	m_stop = true;
	m_pendingPages.clear();
	for (auto *preview : m_boutons) { preview->abort(); }
	m_pages.clear();
	m_images.clear();
	qDeleteAll(m_checkboxes);
	m_checkboxes.clear();

	for (QLayout *layout : qAsConst(m_siteLayouts)) {
		clearLayout(layout);
	}
	qDeleteAll(m_siteLayouts);
	m_siteLayouts.clear();
	m_layouts.clear();

	m_boutons.clear();
}


void SearchTab::setSelectedSources(QSettings *settings)
{
	QStringList sav = settings->value("sites").toStringList();
	for (const QString &key : sav) {
		if (!m_sites.contains(key)) {
			continue;
		}

		m_selectedSources.append(m_sites.value(key));
	}
}

void SearchTab::optionsChanged()
{
	log(QStringLiteral("Updating settings for tab \"%1\".").arg(windowTitle()), Logger::Debug);
	// ui->retranslateUi(this);

	ui_spinImagesPerPage->setValue(m_settings->value("limit", 20).toInt());
	ui_spinColumns->setValue(m_settings->value("columns", 1).toInt());

	/*QPalette p = ui->widgetResults->palette();
	p.setColor(ui->widgetResults->backgroundRole(), QColor(m_settings->value("serverBorderColor", "#000000").toString()));
	ui->widgetResults->setPalette(p);*/
	ui_layoutResults->setHorizontalSpacing(m_settings->value("Margins/main", 10).toInt());
}

void SearchTab::setTagsFromPages(const QMap<QString, QList<QSharedPointer<Page>>> &pages)
{
	// Tags for this page
	QList<Tag> tagList;
	QStringList tagsGot;
	for (const auto &ps : pages) {
		const auto page = ps.last();
		if (!page->isValid()) {
			continue;
		}

		QList<Tag> tags = page->tags();
 		m_completion.append(m_profile->addAutoComplete(tags));
		for (const Tag &tag : tags) {
			if (!tag.text().isEmpty()) {
				// If we already have this tag in the list, we increase its count
				if (tagsGot.contains(tag.text())) {
					const int index = tagsGot.indexOf(tag.text());
					tagList[index].setCount(tagList[index].count() + tag.count());
				} else {
					tagList.append(tag);
					tagsGot.append(tag.text());
				}
			}
		}
	}

	// We sort tags by frequency
	std::sort(tagList.begin(), tagList.end(), sortTagsByCount);

	m_tags = tagList;
	emit tagsChanged();
}

QStringList SearchTab::reasonsToFail(Page *page, const QStringList &completion, QString *meant)
{
	QStringList reasons = QStringList();

	// No valid API
	if (!page->isValid()) {
		reasons.append(tr("invalid or missing login information"));
		return reasons;
	}

	// Filtered images
	if (page->pageImageCount() > 0) {
		reasons.append(tr("all images filtered"));
		return reasons;
	}

	// If the request yielded no source, the server may be offline
	if (!page->hasSource()) {
		reasons.append(tr("server offline"));
	}

	// Some sources do not allow more than two tags per search
	if (page->search().count() > 2) {
		reasons.append(tr("too many tags"));
	}

	// Many sources don't allow browsing after page 1000
	if (page->page() > 1000) {
		reasons.append(tr("page too far"));
	}

	// Auto-correct
	if (meant != nullptr && !page->search().isEmpty()) {
		QMap<QString, QString> results, clean;
		QList<QChar> modifiers { '~', '-' };

		int c = 0;
		for (QString tag : page->search()) {
			QChar modifier;
			if (modifiers.contains(tag[0])) {
				modifier = tag[0];
				tag = tag.mid(1);
			}

			int lev = qCeil((tag.length() - 1) / 4.0);
			for (const QString &comp : completion) {
				// Ignore tags that are too long
				if (abs(comp.length() - tag.length()) > lev) {
					continue;
				}

				const int d = levenshtein(tag, comp);
				if (d < lev) {
					if (results[tag].isEmpty()) {
						c++;
					}
					results[tag] = "<b>" + comp + "</b>";
					clean[tag] = comp;
					lev = d;
				}
			}

			if (lev == 0) {
				results[tag] = tag;
				c--;
			}

			if (!modifier.isNull() && results.contains(tag)) {
				results[tag].prepend(modifier);
				if (clean.contains(tag)) {
					clean[tag].prepend(modifier);
				}
			}
		}

		if (c > 0) {
			QStringList res = results.values(), cl = clean.values();
			*meant = QString(R"(<a href="%1" style="color:black;text-decoration:none;">%2</a>)").arg(cl.join(" ").toHtmlEscaped(), res.join(" "));
		}
	}

	return reasons;
}

void SearchTab::clear()
{
	// Reset loading variables
	m_stop = true;
	m_pendingPages.clear();
	m_failedPages.clear();
	m_filteredImages.clear();
	m_pageMax = -1;
	ui_spinPage->setMaximum(100000);
	m_endlessLoadOffset = 0;

	// Clear page details
	m_tags.clear();
	emit tagsChanged();
	m_wiki.clear();
	emit wikiChanged();

	// Clear layout
	for (int i = 0; i < ui_layoutResults->rowCount(); ++i) {
		ui_layoutResults->setRowMinimumHeight(i, 0);
	}
	for (QLayout *layout : qAsConst(m_siteLayouts)) {
		clearLayout(layout);
	}
	qDeleteAll(m_siteLayouts);
	m_siteLayouts.clear();
	m_layouts.clear();

	for (auto b : m_boutons) {
		b->deleteLater();
	}
	m_boutons.clear();

	qDeleteAll(m_siteLabels);
	m_siteLabels.clear();
	clearLayout(ui_layoutResults);

	// Abort current loadings
	for (const auto &pages : qAsConst(m_pages)) {
		for (const auto &page : pages) {
			page->abort();
			page->abortTags();
		}
	}
	for (auto it = m_thumbnailsLoading.constBegin(); it != m_thumbnailsLoading.constEnd(); ++it) {
		it.key()->abort();
	}

	m_pages.clear();
	m_images.clear();

	m_selectedImagesPtrs.clear();
	m_selectedImages.clear();
	m_thumbnailsLoading.clear();
	m_validImages.clear();
}

TextEdit *SearchTab::createAutocomplete()
{
	auto *ret = new TextEdit(m_profile, this);
	connect(ret, &TextEdit::returnPressed, this, &SearchTab::load);
	connect(ret, &TextEdit::addedFavorite, this, &SearchTab::setFavoriteImage);

	// Add auto-complete if necessary
	if (m_settings->value("autocompletion", true).toBool()) {
		auto *completer = new QCompleter(m_completion, ret);
		completer->setCaseSensitivity(Qt::CaseInsensitive);

		ret->setCompleter(completer);
	}

	return ret;
}

void SearchTab::setMergeResultsMode(bool merged)
{
	// Restore endless loading mode
	if (merged == m_pageMergedMode) {
		setEndlessLoadingMode(m_endlessLoadingEnabledPast);
	}
	// Disable endless loading
	else {
		m_endlessLoadingEnabledPast = m_endlessLoadingEnabled;
		setEndlessLoadingMode(false);
	}
}

void SearchTab::setEndlessLoadingMode(bool enabled)
{
	// Toggle endless loading button
	if (ui_buttonEndlessLoad != nullptr && m_settings->value("infiniteScroll", "disabled") == "button") {
		ui_buttonEndlessLoad->setVisible(enabled);
	}

	m_endlessLoadingEnabled = enabled;
}

void SearchTab::finishedLoading(Page *page)
{
	if (m_stop || !m_pendingPages.contains(page)) {
		return;
	}

	// Filter images depending on tabs
	QList<QSharedPointer<Image>> validImages;
	int filteredImages = 0;
	QString error;
	for (const QSharedPointer<Image> &img : page->images()) {
		if (img->isValid() && validateImage(img, error)) {
			validImages.append(img);
		} else if (!error.isEmpty()) {
			filteredImages++;
			log(error);
		}
	}
	m_validImages.insert(page, validImages);
	m_filteredImages.insert(page, filteredImages + page->filteredImageCount());

	// Remove already existing images for merged results
	const bool merged = ui_checkMergeResults != nullptr && ui_checkMergeResults->isChecked();
	const QList<QSharedPointer<Image>> images = merged ? mergeResults(page->page(), validImages) : validImages;

	m_images.append(images);

	updatePaginationButtons(page);
	addResultsPage(page, images, merged, filteredImages);

	if (!m_settings->value("useregexfortags", true).toBool()) {
		setTagsFromPages(m_pages);
	}

	postLoading(page, images);
}

void SearchTab::failedLoading(Page *page)
{
	if (m_stop || !m_pendingPages.contains(page)) { return; }
	m_failedPages.insert(page);
	const bool merged = ui_checkMergeResults != nullptr && ui_checkMergeResults->isChecked();
	addResultsPage(page, {}, merged, 0, tr("Source failed. Reload to retry."));
	postLoading(page, {});
}

void SearchTab::httpsRedirect(Page *page)
{
	QSettings *settings = m_profile->getSettings();

	const QString action = settings->value("ssl_autocorrect", "ask").toString();
	bool setSsl = action == "always";

	if (action == "ask") {
		QMessageBox box(this);
		box.setWindowTitle(tr("HTTPS redirection detected"));
		box.setText(tr("An HTTP to HTTPS redirection has been detected for the website %1. Do you want to enable SSL on it? The recommended setting is 'yes'.").arg(page->site()->url()));
		QPushButton *yes = box.addButton(QMessageBox::Yes);
		QPushButton *always = box.addButton(tr("Always"), QMessageBox::YesRole);
		QPushButton *neverWebsite = box.addButton(tr("Never for that website"), QMessageBox::NoRole);
		QPushButton *never = box.addButton(tr("Never"), QMessageBox::NoRole);
		box.exec();

		if (box.clickedButton() == yes) {
			setSsl = true;
		} else if (box.clickedButton() == always) {
			setSsl = true;
			settings->setValue("ssl_autocorrect", "always");
		} else if (box.clickedButton() == neverWebsite) {
			page->site()->setSetting("ssl_never_correct", true, false);
		} else if (box.clickedButton() == never) {
			settings->setValue("ssl_autocorrect", "never");
		}
	}

	if (setSsl) {
		log(QStringLiteral("[%1] Enabling HTTPS").arg(page->site()->url()), Logger::Info);
		page->site()->setSetting("ssl", true, false);
	}
}

void SearchTab::postLoading(Page *page, const QList<QSharedPointer<Image>> &images)
{
	m_pendingPages.remove(page);
	++m_page;
	const bool merged = ui_checkMergeResults != nullptr && ui_checkMergeResults->isChecked();
	const bool finished = m_pendingPages.isEmpty();

	if (merged) {
		// Increase the progress bar status
		if (ui_progressMergeResults != nullptr) {
			ui_progressMergeResults->setValue(ui_progressMergeResults->value() + 1);
		}

		// Hide progress bar when we load the last page
		if (ui_stackedMergeResults != nullptr && finished) {
			ui_stackedMergeResults->setCurrentIndex(1);
		}

		// Create the label when loading the first page
		if (m_page == 1 && m_siteLabels.isEmpty()) {
			auto *txt = new QLabel(this);
			txt->setOpenExternalLinks(true);
			setMergedLabelText(txt, m_images);
			m_siteLabels.insert(nullptr, txt);

			ui_layoutResults->addWidget(txt, 0, 0);
			ui_layoutResults->setRowMinimumHeight(0, txt->sizeHint().height() + 10);
		} else if (!m_siteLabels.isEmpty()) {
			setMergedLabelText(m_siteLabels[nullptr], m_images);
		}
	}

	// Load thumbnails
	for (const auto &img : images) {
		addResultsImage(img, page, merged);
	}

	updatePaginationButtons(page);
	if (finished) {
		bool more = false;
		for (const auto &pages : m_pages) {
			if (!pages.isEmpty() && !m_failedPages.contains(pages.last().data()) && pages.last()->isValid() && pages.last()->hasNext()) { more = true; }
		}
		setEndlessLoadingMode(more);
	}

	ui_buttonGetAll->setDisabled(m_images.empty());
	ui_buttonGetPage->setDisabled(m_images.empty());
	ui_buttonGetSel->setDisabled(m_images.empty());
}

void SearchTab::updatePaginationButtons(Page *page)
{
	Q_UNUSED(page)
	bool more = false, exact = !m_pages.isEmpty(), jumpable = true;
	int last = 1;
	for (const auto &pages : m_pages) {
		if (pages.isEmpty()) { exact = false; continue; }
		const auto &latest = pages.last();
		if (!latest->isValid() || m_failedPages.contains(latest.data())) { exact = false; continue; }
		more = more || latest->hasNext();
		jumpable = jumpable && latest->pageInformation().nextPage.isEmpty();
		const int count = latest->pagesCount(false);
		if (count < 0 || (latest->hasNext() && count <= latest->page())) { exact = false; }
		else { last = qMax(last, count); }
	}
	m_pageMax = exact && jumpable ? last : -1;
	ui_spinPage->setMaximum(exact ? qMax(ui_spinPage->value(), last) : 100000);
	const bool idle = m_pendingPages.isEmpty();
	ui_buttonNextPage->setEnabled(idle && more);
	ui_buttonLastPage->setEnabled(idle && exact && jumpable && last > ui_spinPage->value());
}

void SearchTab::finishedLoadingTags(Page *page)
{
	const auto pages = m_pages.value(page->website());
	if (m_stop || std::none_of(pages.cbegin(), pages.cend(), [page](const QSharedPointer<Page> &known) { return known.data() == page; })) { return; }
	setTagsFromPages(m_pages);

	// Wiki
	if (!page->wiki().isEmpty()) {
		m_wiki = page->wiki();
		emit wikiChanged();
	}

	updatePaginationButtons(page);

	// Update image and page count
	QList<QSharedPointer<Image>> images;
	int filteredImages = 0;
	QString error;
	for (const QSharedPointer<Image> &img : page->images()) {
		if (img->isValid() && validateImage(img, error)) {
			images.append(img);
		} else {
			filteredImages++;
		}
	}

	if (ui_checkMergeResults != nullptr && ui_checkMergeResults->isChecked() && m_siteLabels.contains(nullptr)) {
		setMergedLabelText(m_siteLabels[nullptr], m_images);
	} else if (m_siteLabels.contains(page->site())) {
		setPageLabelText(m_siteLabels[page->site()], page, images, filteredImages);
	}
}

void SearchTab::finishedLoadingPreview()
{
	auto *preview = qobject_cast<ImagePreview*>(sender());

	if (m_stop) {
		return;
	}

	// Try to find associated image
	QSharedPointer<Image> img;
	if (m_thumbnailsLoading.contains(preview)) {
		img = m_thumbnailsLoading[preview];
		m_thumbnailsLoading.remove(preview);
	} else {
		if (m_boutons.values().contains(preview)) { return; } // A manual thumbnail retry has no pending first-load work.
		log(QStringLiteral("Could not find image related to loaded thumbnail"), Logger::Error);
		return;
	}

	// Download whitelist images on thumbnail view
	Blacklist whitelistedTags;
	for (const QString &tag : m_settings->value("whitelistedtags").toString().split(" ", Qt::SkipEmptyParts)) {
		whitelistedTags.add(tag);
	}
	QStringList detected = m_profile->getBlacklist().match(img->tokens(m_profile));
	QStringList whitelisted = whitelistedTags.match(img->tokens(m_profile));
	if (!whitelisted.isEmpty() && m_settings->value("whitelist_download", "image").toString() == "page") {
		bool download = false;
		if (!detected.isEmpty()) {
			const int answer = QMessageBox::question(this, "Grabber", tr("Some tags from the image are in the whitelist: %1. However, some tags are in the blacklist: %2. Do you want to download it anyway?").arg(whitelisted.join(", "), detected.join(", ")), QMessageBox::Yes | QMessageBox::Open | QMessageBox::No);
			if (answer == QMessageBox::Yes) {
				download = true;
			} else if (answer == QMessageBox::Open) {
				openImage(img);
			}
		} else {
			download = true;
		}

		if (download) {
			auto downloader = new ImageDownloader(m_profile, img, m_settings->value("Save/filename").toString(), m_settings->value("Save/path").toString(), 1, true, true, this);
			m_downloadQueue->add(DownloadQueue::Background, downloader);
		}
	}
}

QList<QSharedPointer<Image>> SearchTab::mergeResults(int page, const QList<QSharedPointer<Image>> &results)
{
	Q_UNUSED(page)
	// Only exact checksums or identical full file URLs collapse. Conflicting
	// checksums at one URL retain edits; no visual-similarity deduplication.
	static const QRegularExpression checksum("^[0-9a-fA-F]{32}$");
	QSet<QString> hashes;
	QHash<QString, QSet<QString>> urls;
	auto hash = [](const QSharedPointer<Image> &image) {
		const QString md5 = image->md5().trimmed();
		return checksum.match(md5).hasMatch() ? md5.toLower() : QString();
	};
	auto remember = [&hashes, &urls, &hash](const QSharedPointer<Image> &image) {
		const QString md5 = hash(image);
		if (!md5.isEmpty()) { hashes.insert(md5); }
		if (!image->url().isEmpty()) { urls[image->url().toString(QUrl::FullyEncoded)].insert(md5); }
	};
	for (const auto &image : m_images) { remember(image); }
	QList<QSharedPointer<Image>> unique;
	for (const auto &image : results) {
		const QString md5 = hash(image);
		bool duplicate = !md5.isEmpty() && hashes.contains(md5);
		const QString url = image->url().toString(QUrl::FullyEncoded);
		if (!duplicate && !url.isEmpty() && urls.contains(url)) {
			const auto known = urls.value(url);
			duplicate = md5.isEmpty() && known.size() == 1 && known.contains(QString());
		}
		if (!duplicate) { unique.append(image); remember(image); }
	}
	return unique;
}

void SearchTab::addResultsPage(Page *page, const QList<QSharedPointer<Image>> &images, bool merged, int filteredImages, const QString &noResultsMessage)
{
	if (merged) {
		return;
	}

	const int pos = m_pages.keys().indexOf(page->website());
	if (pos < 0) {
		return;
	}

	const int page_x = pos % ui_spinColumns->value();
	const int page_y = (pos / ui_spinColumns->value()) * 2;

	Site *site = page->site();
	if (!m_siteLabels.contains(site)) {
		auto *txt = new QLabel(this);
		txt->setOpenExternalLinks(true);
		m_siteLabels.insert(site, txt);

		ui_layoutResults->addWidget(txt, page_y, page_x);
		ui_layoutResults->setRowMinimumHeight(page_y, txt->sizeHint().height() + 10);
	}
	setPageLabelText(m_siteLabels[site], page, images, filteredImages, noResultsMessage);

	if (m_siteLayouts.contains(page->site()) && m_pages.value(page->website()).count() == 1) {
		addLayout(m_siteLayouts[page->site()], page_y + 1, page_x);
	}
}
void SearchTab::setMergedLabelText(QLabel *txt, const QList<QSharedPointer<Image>> &images)
{
	int firstPage = ui_spinPage->value(), lastPage = firstPage;
	qint64 sourceTotal = 0;
	bool known = !m_pages.isEmpty(), estimated = false;
	QStringList links, failures;
	for (const auto &pages : m_pages) {
		if (pages.isEmpty()) { continue; }
		const auto &latest = pages.last();
		for (const auto &page : pages) { firstPage = qMin(firstPage, page->page()); lastPage = qMax(lastPage, page->page()); }
		const QString name = latest->site()->name().toHtmlEscaped();
		links.append(QStringLiteral("<a href=\"%1\">%2</a>").arg(latest->url().toString().toHtmlEscaped(), name));
		if (m_failedPages.contains(latest.data())) { failures.append(tr("%1 failed; reload to retry").arg(name)); }
		const int count = latest->imagesCount();
		if (count < 0 || (latest->hasNext() && latest->pagesCount(false) >= 0 && latest->pagesCount(false) <= latest->page()) || !latest->isValid() || m_failedPages.contains(latest.data())) { known = false; }
		else { sourceTotal += count; estimated = estimated || latest->imagesCount(false) < 0; }
	}
	const QString page = firstPage != lastPage ? QStringLiteral("%1–%2").arg(firstPage).arg(lastPage) : QString::number(lastPage);
	QString label = links.join(", ") + " · " + tr("Page %1 · %2 unique images shown").arg(page).arg(images.size());
	if (known) { label += " · " + tr("%1 source results before merging").arg((estimated ? "~" : QString()) + QString::number(sourceTotal)); }
	else { label += " · " + tr("Total unknown"); }
	if (!failures.isEmpty()) { label += "<br/>" + failures.join("<br/>"); }
	txt->setText(label);
}

void SearchTab::setPageLabelText(QLabel *txt, Page *page, const QList<QSharedPointer<Image>> &images, int filteredImages, const QString &noResultsMessage)
{
	// No results message
	if (images.isEmpty()) {
		QString meant;
		QStringList reasons = reasonsToFail(page, m_completion, &meant);
		if (!meant.isEmpty() && ui_widgetMeant != nullptr) {
			ui_widgetMeant->show();
			ui_labelMeant->setText(meant);
		}

		const QString name = page->isValid() ? QStringLiteral("<a href=\"%1\">%2</a>").arg(page->url().toString().toHtmlEscaped(), page->site()->name().toHtmlEscaped()) : page->site()->name().toHtmlEscaped();
		const QString msg = noResultsMessage == nullptr ? tr("No result") : noResultsMessage;
		txt->setText(name + " - " + msg + (reasons.count() > 0 ? "<br/>" + tr("Possible reasons: %1").arg(reasons.join(", ")) : QString()));
		return;
	}

	const int pageCount = page->pagesCount();
	const int imageCount = page->imagesCount();

	int firstPage = images.count() > 0 ? page->page() : 0;
	int lastPage = images.count() > 0 ? page->page() : 0;
	int totalCount = 0;
	filteredImages = 0;
	for (const QSharedPointer<Page> &p : m_pages[page->website()]) {
		if (p->images().count() == 0) {
			continue;
		}
		if (p->page() < firstPage || firstPage == 0) {
			firstPage = p->page();
		}
		if (p->page() > lastPage) {
			lastPage = p->page();
		}
		totalCount += m_validImages.value(p.data()).count();
		filteredImages += m_filteredImages.value(p.data());
	}

	const QString pageLabel = firstPage != lastPage ? QString("%1-%2").arg(firstPage).arg(lastPage) : QString::number(lastPage);
	const QString pageCountStr = pageCount > 0
		? (page->pagesCount(false) == -1 ? "~" : QString()) + QString::number(pageCount)
		: (page->maxPagesCount() == -1 ? "?" : tr("max %1").arg(page->maxPagesCount()));
	const QString imageCountStr = imageCount > 0
		? (page->imagesCount(false) == -1 ? "~" : QString()) + QString::number(imageCount)
		: (page->maxImagesCount() == -1 ? "?" : tr("max %1").arg(page->maxImagesCount()));

	const QString countLabel = tr("Page %1 of %2 (%3 of %4)").arg(pageLabel, pageCountStr).arg(totalCount).arg(imageCountStr);
	QString label = "<a href=\"" + page->url().toString().toHtmlEscaped() + "\">" + page->site()->name().toHtmlEscaped() + "</a> - " + countLabel;

	// Filtered images count
	if (filteredImages > 0 && m_settings->value("showFilteredImagesCount", true).toBool()) {
		label += " - " + tr("%1 filtered").arg(filteredImages);
	}

	txt->setText(label);

	/*if (page->search().join(" ") != m_search->toPlainText() && m_settings->value("showtagwarning", true).toBool()) {
		QStringList uncommon = m_search->toPlainText().toLower().trimmed().split(" ", Qt::SkipEmptyParts);
		uncommon.append(m_settings->value("add").toString().toLower().trimmed().split(" ", Qt::SkipEmptyParts));
		for (int i = 0; i < page->search().size(); i++) {
			if (uncommon.contains(page->search().at(i))) {
				uncommon.removeAll(page->search().at(i));
			}
		}
		if (!uncommon.isEmpty()) {
			txt->setText(txt->text()+"<br/>"+QString(tr("Des modificateurs ont été otés de la recherche car ils ne sont pas compatibles avec cet imageboard : %1.")).arg(uncommon.join(" ")));
		}
	}*/

	// Show warnings
	if (!page->errors().isEmpty() && m_settings->value("showwarnings", true).toBool()) {
		txt->setText(txt->text() + "<br/>" + page->errors().join("\n").toHtmlEscaped().replace("\n", "<br/>"));
	}
}

QWidget *SearchTab::createImageThumbnail()
{
	auto *widget = new QWidget(this);
	const int sizes[] = {128, 180, 256};
	const int imageSize = m_settings->contains("Gallery/density")
		? sizes[qBound(0, m_settings->value("Gallery/density").toInt(), 2)]
		: qBound(32, qFloor(FIXED_IMAGE_WIDTH * qBound(0.25, m_settings->value("thumbnailUpscale", 1.0).toDouble(), 3.4)), 512);
	const int dim = imageSize + 2 * qBound(0, m_settings->value("borders", 3).toInt(), 16);
	widget->setFixedSize(dim, dim);
	return widget;
}

void SearchTab::thumbnailContextMenu(QMenu *menu, const QSharedPointer<Image> &img)
{
	Q_UNUSED(img)

	QAction *first = menu->actions().first();

	// Save selected
	if (!m_selectedImagesPtrs.empty()) {
		auto *actionSaveSelected = new QAction(QIcon(":/images/icons/save.png"), tr("Save selected"), menu);
		connect(actionSaveSelected, &QAction::triggered, this, &SearchTab::contextSaveSelected);
		menu->insertAction(first, actionSaveSelected);
	}
}

void SearchTab::contextSaveSelected()
{
	const QString fn = m_settings->value("Save/filename").toString();
	const QString path = m_settings->value("Save/path").toString();

	for (const QSharedPointer<Image> &img : qAsConst(m_selectedImagesPtrs)) {
		auto *downloader = new ImageDownloader(m_profile, img, fn, path, 1, true, true, this);
		if (m_boutons.contains(img.data())) {
			connect(downloader, &ImageDownloader::downloadProgress, [this](const QSharedPointer<Image> &img, qint64 v1, qint64 v2) {
				ImagePreview *preview = m_boutons.value(img.data(), nullptr);
				if (preview != nullptr) {
					preview->setDownloadProgress(v1, v2);
				}
			});
		}
		m_downloadQueue->add(DownloadQueue::Manual, downloader);
	}
}


QList<QSharedPointer<Page>> SearchTab::getPagesToDownload()
{
	const bool unloaded = m_settings->value("getunloadedpages", false).toBool();

	QList<QSharedPointer<Page>> pages;
	if (unloaded) {
		QStringList keys = m_sites.keys();
		for (int i = 0; i < m_checkboxes.count(); i++) {
			if (m_checkboxes[i]->isChecked() && m_pages.contains(keys[i])) {
				pages.append(m_pages[keys[i]].first());
			}
		}
	} else {
		for (auto it = m_pages.begin(); it != m_pages.end(); ++it) {
			pages.append(it.value().first());
		}
	}

	return pages;
}

void SearchTab::addResultsImage(const QSharedPointer<Image> &img, Page *page, bool merge)
{
	// Skip invalid images (placeholders and similar)
	if (!img->isValid()) {
		return;
	}

	// Early return if the layout has already been removed
	Page *layoutKey = merge && m_layouts.contains(nullptr) ? nullptr : page;
	if (!m_layouts.contains(layoutKey)) {
		return;
	}

	// Keep the displayed image and viewer binding at the same stable position.
	const int absolutePosition = m_images.indexOf(img);
	if (absolutePosition < 0) { return; }

	// Calculate relative position compared to validated images
	int relativePosition = merge
		? absolutePosition
		: m_validImages[page].indexOf(img);

	auto *widget = createImageThumbnail();

	auto *preview = new ImagePreview(img, widget, m_profile, m_downloadQueue, m_parent, this);
	preview->setCustomContextMenu([this](QMenu *menu, const QSharedPointer<Image> &img) { this->thumbnailContextMenu(menu, img); });
	m_boutons.insert(img.data(), preview);
	m_thumbnailsLoading.insert(preview, img);

	FixedSizeGridLayout *layout = m_layouts[layoutKey];
	layout->insertWidget(relativePosition, widget);

	connect(preview, &ImagePreview::finished, this, &SearchTab::finishedLoadingPreview);
	connect(preview, &ImagePreview::clicked, [this, absolutePosition]() { this->openImage(absolutePosition); });
	connect(preview, &ImagePreview::toggled, [this, absolutePosition](bool toggle, bool range) { this->toggleImage(absolutePosition, toggle, range); });
	preview->load();
}

void SearchTab::addHistory(const SearchQuery &query, int page, int ipp, int cols)
{
	QMap<QString, QString> history;
	if (!query.gallery.isNull()) {
		history["gallery"] = query.gallery->name();
	} else {
		history["tags"] = query.tags.join(' ');
	}
	history["page"] = QString::number(page);
	history["ipp"] = QString::number(ipp);
	history["columns"] = QString::number(cols);
	m_history.append(history);

	if (m_history.size() > 1) {
		m_history_cursor++;
		ui_buttonHistoryBack->setEnabled(true);
		ui_buttonHistoryNext->setEnabled(false);
	}
}
void SearchTab::historyBack()
{
	if (m_history_cursor <= 0) {
		return;
	}

	m_from_history = true;
	m_history_cursor--;

	ui_spinPage->setValue(m_history[m_history_cursor]["page"].toInt());
	ui_spinImagesPerPage->setValue(m_history[m_history_cursor]["ipp"].toInt());
	ui_spinColumns->setValue(m_history[m_history_cursor]["columns"].toInt());
	setTags(m_history[m_history_cursor]["tags"]);

	ui_buttonHistoryNext->setEnabled(true);
	if (m_history_cursor == 0) {
		ui_buttonHistoryBack->setEnabled(false);
	}
}
void SearchTab::historyNext()
{
	if (m_history_cursor >= m_history.size() - 1) {
		return;
	}

	m_from_history = true;
	m_history_cursor++;

	ui_spinPage->setValue(m_history[m_history_cursor]["page"].toInt());
	ui_spinImagesPerPage->setValue(m_history[m_history_cursor]["ipp"].toInt());
	ui_spinColumns->setValue(m_history[m_history_cursor]["columns"].toInt());
	setTags(m_history[m_history_cursor]["tags"]);

	ui_buttonHistoryBack->setEnabled(true);
	if (m_history_cursor == m_history.size() - 1) {
		ui_buttonHistoryNext->setEnabled(false);
	}
}

void SearchTab::getSel()
{
	if (m_selectedImagesPtrs.empty()) {
		return;
	}

	for (const QSharedPointer<Image> &img : qAsConst(m_selectedImagesPtrs)) {
		emit batchAddUnique(DownloadQueryImage(m_settings, img, img->parentSite()));
	}

	m_selectedImagesPtrs.clear();
	m_selectedImages.clear();
	for (auto *l : qAsConst(m_boutons)) {
		l->setChecked(false);
	}
}

void SearchTab::updateCheckboxes()
{
	if (ui_layoutSourcesList == nullptr) {
		return;
	}

	log(QStringLiteral("Updating checkboxes."));

	qDeleteAll(m_checkboxes);
	m_checkboxes.clear();

	const int n = m_settings->value("Sources/Letters", 3).toInt();
	int m = n;

	for (auto it = m_sites.constBegin(); it != m_sites.constEnd(); ++it) {
		Site *site = it.value();
		QString url = site->url();

		if (url.startsWith("www.")) {
			url = url.right(url.length() - 4);
		} else if (url.startsWith("chan.")) {
			url = url.right(url.length() - 5);
		}

		if (n < 0) {
			m = url.indexOf('.');
			if (n < -1 && url.indexOf('.', m + 1) != -1) {
				m = url.indexOf('.', m + 1);
			}
		}

		auto *checkBox = new QCheckBox(url.left(m), this);
			checkBox->setChecked(m_selectedSources.contains(site));
			m_checkboxesSignalMapper->setMapping(checkBox, it.key());
			connect(checkBox, SIGNAL(toggled(bool)), m_checkboxesSignalMapper, SLOT(map()));
			ui_layoutSourcesList->addWidget(checkBox);

		m_checkboxes.append(checkBox);
	}

	DONE();
}

void SearchTab::openImage(int id)
{
	if (id < 0 || id >= m_images.count()) {
		return;
	}

	const QSharedPointer<Image> &image = m_images.at(id);

	if (m_settings->value("warnblacklisted", true).toBool()) {
		QStringList detected = m_profile->getBlacklist().match(image->tokens(m_profile));
		if (!detected.isEmpty()) {
			const int reply = QMessageBox::question(parentWidget(), tr("Blacklist"), tr("%n tag figuring in the blacklist detected in this image: %1. Do you want to display it anyway?", "", detected.size()).arg(detected.join(", ")), QMessageBox::Yes | QMessageBox::No);
			if (reply == QMessageBox::No) {
				return;
			}
		}
	}

	openImage(image);
}

void SearchTab::openImage(const QSharedPointer<Image> &image)
{
	if (image->isGallery()) {
		m_parent->addGalleryTab(image->parentSite(), image);
		return;
	}

	if (m_settings->value("Viewer/singleWindow", false).toBool() && !m_lastViewerWindow.isNull()) {
		m_lastViewerWindow->reuse(m_images, image, image->parentSite());
		m_lastViewerWindow->activateWindow();
		return;
	}

	auto *viewer = new ViewerWindow(m_images, image, image->parentSite(), m_profile, m_parent, this);
	connect(viewer, SIGNAL(linkClicked(QString)), this, SLOT(setTags(QString)));
	connect(viewer, SIGNAL(poolClicked(int, QString)), m_parent, SLOT(addPoolTab(int, QString)));
	viewer->show();

	m_lastViewerWindow = viewer;
}


void SearchTab::mousePressEvent(QMouseEvent *e)
{
	if (e->button() == Qt::XButton1) {
		previousPage();
	} else if (e->button() == Qt::XButton2) {
		nextPage();
	}
}


void SearchTab::selectImage(const QSharedPointer<Image> &img)
{
	if (!m_selectedImagesPtrs.contains(img)) {
		m_selectedImagesPtrs.append(img);
		m_selectedImages.append(img->url());
	}
}

void SearchTab::unselectImage(const QSharedPointer<Image> &img)
{
	if (m_selectedImagesPtrs.contains(img)) {
		const int pos = m_selectedImagesPtrs.indexOf(img);
		m_selectedImagesPtrs.removeAt(pos);
		m_selectedImages.removeAt(pos);
	}
}

void SearchTab::toggleImage(const QSharedPointer<Image> &img)
{
	// Sometimes happen with range selection when an image hasn't loaded yet
	if (!m_boutons.contains(img.data())) {
		return;
	}

	const bool selected = m_selectedImagesPtrs.contains(img);
	m_boutons[img.data()]->setChecked(!selected);

	if (selected) {
		const int pos = m_selectedImagesPtrs.indexOf(img);
		m_selectedImagesPtrs.removeAt(pos);
		m_selectedImages.removeAt(pos);
	} else {
		m_selectedImagesPtrs.append(img);
		m_selectedImages.append(img->url());
	}
}

void SearchTab::toggleImage(int id, bool toggle, bool range)
{
	if (toggle) {
		selectImage(m_images[id]);
	} else {
		unselectImage(m_images[id]);
	}

	if (range) {
		if (id > m_lastToggle) {
			for (int i = m_lastToggle + 1; i < id; ++i) {
				toggleImage(m_images[i]);
			}
		} else {
			for (int i = m_lastToggle - 1; i > id; --i) {
				toggleImage(m_images[i]);
			}
		}
	}

	m_lastToggle = id;
}



void SearchTab::openSourcesWindow()
{
	auto *sourcesWindow = new SourcesWindow(m_profile, m_selectedSources, this);
	connect(sourcesWindow, SIGNAL(valid(QList<Site*>)), this, SLOT(saveSources(QList<Site*>)));
	sourcesWindow->show();
}

QList<Site*> SearchTab::sourcesWithResults(bool eager)
{
	if (m_images.isEmpty() && !eager) {
		return m_selectedSources;
	}

	QSet<Site*> ret;
	for (const auto &img : qAsConst(m_images)) {
		ret.insert(img->parentSite());
	}
	return ret.values();
}

void SearchTab::pruneSources()
{
	if (m_images.isEmpty()) {
		return;
	}

	log(QStringLiteral("Pruning sources..."), Logger::Info);

	QSet<Site*> sitesWithImages;
	for (const QSharedPointer<Image> &img : qAsConst(m_images)) {
		if (!sitesWithImages.contains(img->parentSite())) {
			sitesWithImages.insert(img->parentSite());
		}
	}

	QList<Site*> goodSources;
	QStringList removedSources;
	for (Site *site : m_selectedSources) {
		if (sitesWithImages.contains(site)) {
			goodSources.append(site);
		} else {
			removedSources.append(site->name());
		}
	}

	this->setSources(goodSources);
	log(QStringLiteral("Sources pruned: %1").arg(removedSources.isEmpty() ? "none" : removedSources.join(", ")), Logger::Info);
}


void SearchTab::saveSources(const QList<Site*> &sel, bool canLoad)
{
	log(QStringLiteral("Saving sources..."));

	// Reset page counter when adding a new source
	for (Site *site : sel) {
		if (!m_selectedSources.contains(site)) {
			ui_spinPage->setValue(1);
		}
	}

	QStringList sav;
	sav.reserve(sel.count());
	for (Site *enabled : sel) {
		sav.append(enabled->url());
	}
	m_settings->setValue("sites", sav);
	setSources(sel);

	// Log into new sources
	for (Site *site : sel) {
		site->login();
	}

	DONE();

	if (m_history.isEmpty() && canLoad) {
		load();
	}
}


void SearchTab::loadTags(SearchQuery query)
{
	log(QStringLiteral("Loading results..."));

	// Save history
	m_profile->getHistory()->addQuery(query, loadSites());

	// Enable or disable scroll mode
	const bool resultsScrollArea = m_settings->value("resultsScrollArea", true).toBool();
	ui_scrollAreaResults->setScrollEnabled(resultsScrollArea);

	// Append "additional tags" setting
	if (query.gallery.isNull()) {
		query.tags.append(m_settings->value("add").toString().trimmed().split(" ", Qt::SkipEmptyParts));
	}

	// Save previous pages
	m_lastPages.clear();
	for (Site *sel : qAsConst(m_selectedSources)) {
		const QString &site = sel->url();
		if (query == m_lastQuery && m_lastRequestedLimit == ui_spinImagesPerPage->value() && m_pages.contains(site)) {
			m_lastPages.insert(site, m_pages[site].last());
		}
	}

	clear();

	// Disable download buttons
	ui_buttonGetAll->setEnabled(false);
	ui_buttonGetPage->setEnabled(false);
	ui_buttonGetSel->setEnabled(false);

	// Disable pagination buttons
	ui_buttonNextPage->setEnabled(false);
	ui_buttonLastPage->setEnabled(false);

	if (!m_from_history) {
		addHistory(query, ui_spinPage->value(), ui_spinImagesPerPage->value(), ui_spinColumns->value());
	}
	m_from_history = false;

	if (m_hasLastQuery && query != m_lastQuery && m_history_cursor == m_history.size() - 1) {
		ui_spinPage->setValue(1);
	}
	m_lastRequestedLimit = ui_spinImagesPerPage->value();
	m_lastQuery = query;
	m_hasLastQuery = true;

	if (ui_widgetMeant != nullptr) {
		ui_widgetMeant->hide();
	}
	ui_buttonFirstPage->setEnabled(ui_spinPage->value() > 1);
	ui_buttonPreviousPage->setEnabled(ui_spinPage->value() > 1);

	const bool merged = ui_checkMergeResults != nullptr && ui_checkMergeResults->isChecked();
	m_pageMergedMode = merged;
	if (merged) {
		m_layouts.insert(nullptr, createImagesLayout(m_settings));
	}

	loadPage();

	emit changed(this);
}

void SearchTab::endlessLoad()
{
	if (!m_endlessLoadingEnabled) {
		return;
	}

	const bool rememberPage = m_settings->value("infiniteScrollRememberPage", false).toBool();

	if (rememberPage) {
		ui_spinPage->setValue(ui_spinPage->value() + 1);
	} else {
		m_endlessLoadOffset++;
	}

	loadPage();
}

void SearchTab::loadPage()
{
	if (!m_pendingPages.isEmpty()) { return; }
	const bool merged = ui_checkMergeResults != nullptr && ui_checkMergeResults->isChecked();
	const int perPage = ui_spinImagesPerPage->value();
	const int currentPage = ui_spinPage->value() + m_endlessLoadOffset;
	setEndlessLoadingMode(false);
	ui_buttonNextPage->setEnabled(false);
	ui_buttonLastPage->setEnabled(false);
	m_page = 0;
	m_stop = false;
	QList<QSharedPointer<Page>> batch;
	for (Site *site : loadSites()) {
		QSharedPointer<Page> previous = m_pages.value(site->url()).isEmpty() ? m_lastPages.value(site->url()) : m_pages.value(site->url()).last();
		if (previous && !m_pages.value(site->url()).isEmpty() && !m_failedPages.contains(previous.data()) && previous->isValid() && !previous->hasNext()) { continue; }
		if (previous && previous->page() + 1 != currentPage) { previous.clear(); }
		SearchQuery query = m_lastQuery;
		if (m_lastUrls.contains(site->url())) { query.urls = m_lastUrls.take(site->url()); }
		auto page = QSharedPointer<Page>::create(m_profile, site, m_sites.values(), query, currentPage, perPage, postFilter(true), false, this);
		if (previous) { page->setLastPage(previous->pageInformation()); }
		connect(page.data(), &Page::finishedLoading, this, &SearchTab::finishedLoading);
		connect(page.data(), &Page::failedLoading, this, &SearchTab::failedLoading);
		connect(page.data(), &Page::httpsRedirect, this, &SearchTab::httpsRedirect);
		m_pages[site->url()].append(page);
		m_pendingPages.insert(page.data());
		batch.append(page);
		if (!merged) {
			auto *pageLayout = createImagesLayout(m_settings);
			m_layouts.insert(page.data(), pageLayout);
			if (!m_siteLayouts.contains(site)) { m_siteLayouts.insert(site, new QVBoxLayout()); }
			m_siteLayouts[site]->addLayout(pageLayout);
		}
	}
	if (merged && m_layouts.contains(nullptr) && m_layouts.value(nullptr)->parentWidget() == nullptr) { addLayout(m_layouts.value(nullptr), 1, 0); }
	if (merged && ui_progressMergeResults != nullptr) {
		ui_progressMergeResults->setMaximum(qMax(1, int(batch.size())));
		ui_progressMergeResults->setValue(0);
	}
	if (ui_stackedMergeResults != nullptr) { ui_stackedMergeResults->setCurrentIndex(merged && !batch.isEmpty() ? 0 : 1); }
	// Register the complete batch before requests can deliver cached callbacks.
	for (const auto &page : batch) {
		if (!page->isValid()) { failedLoading(page.data()); continue; }
		if (m_settings->value("useregexfortags", true).toBool()) {
			connect(page.data(), &Page::finishedLoadingTags, this, &SearchTab::finishedLoadingTags);
			page->loadTags();
		}
		page->load();
	}
	if (batch.isEmpty()) { updatePaginationButtons(nullptr); }
}

void SearchTab::addLayout(QLayout *layout, int row, int column)
{
	auto *layoutWidget = new QWidget;
	layoutWidget->setLayout(layout);
	ui_layoutResults->addWidget(layoutWidget, row, column);
}

FixedSizeGridLayout *SearchTab::createImagesLayout(QSettings *settings)
{
	const int hSpace = settings->value("Margins/horizontal", 6).toInt();
	const int vSpace = settings->value("Margins/vertical", 6).toInt();
	auto *l = new FixedSizeGridLayout(hSpace, vSpace);

	const int sizes[] = {128, 180, 256};
	const int imageSize = settings->contains("Gallery/density")
		? sizes[qBound(0, settings->value("Gallery/density").toInt(), 2)]
		: qBound(32, qFloor(FIXED_IMAGE_WIDTH * qBound(0.25, settings->value("thumbnailUpscale", 1.0).toDouble(), 3.4)), 512);
	l->setFixedWidth(imageSize + 2 * qBound(0, settings->value("borders", 3).toInt(), 16));

	return l;
}


bool SearchTab::validateImage(const QSharedPointer<Image> &img, QString &error)
{
	QStringList detected = m_profile->getBlacklist().match(img->tokens(m_profile));
	if (!detected.isEmpty() && m_settings->value("hideblacklisted", false).toBool()) {
		error = QStringLiteral("Image #%1 ignored. Reason: %2.").arg(img->id()).arg("\"" + detected.join(", ") + "\"");
		return false;
	}

	return true;
}

QList<Site*> SearchTab::loadSites() const
{ return m_selectedSources; }


void SearchTab::setSources(const QList<Site*> &sources)
{
	m_selectedSources = sources;
	updateCheckboxes();
}
void SearchTab::toggleSource(const QString &url)
{
	Site *site = m_sites.value(url);

	const int removed = m_selectedSources.removeAll(site);
	if (removed == 0) {
		m_selectedSources.append(site);
	}
}
void SearchTab::setFavoriteImage(const QString &name)
{
	// When all images are filtered or if there are no results, we can't use any thumbnail for the new favorite
	if (m_images.isEmpty()) {
		return;
	}

	for (Favorite &fav : m_favorites) {
		if (fav.getName() == name) {
			fav.setImage(m_images.first()->previewImage());
			m_profile->emitFavorite();
			return;
		}
	}
}

QList<Site*> SearchTab::sources()
{ return m_selectedSources; }

const QList<Tag> &SearchTab::results() const
{ return m_tags; }
const QString &SearchTab::wiki() const
{ return m_wiki; }

void SearchTab::onLoad()
{}


void SearchTab::firstPage()
{
	ui_spinPage->setValue(1);
	load();
}
void SearchTab::previousPage()
{
	if (ui_spinPage->value() > 1) {
		ui_spinPage->setValue(ui_spinPage->value() - 1);
		load();
	}
}
void SearchTab::nextPage()
{
	if (ui_spinPage->value() < ui_spinPage->maximum()) {
		ui_spinPage->setValue(ui_spinPage->value() + 1);
		load();
	}
}
void SearchTab::lastPage()
{
	if (m_pageMax < 1) { return; }
	ui_spinPage->setValue(m_pageMax);
	load();
}

void SearchTab::setImagesPerPage(int ipp)
{ ui_spinImagesPerPage->setValue(ipp); }
void SearchTab::setColumns(int columns)
{ ui_spinColumns->setValue(columns); }
void SearchTab::setPostFilter(const QStringList &postFilter)
{ m_postFiltering->setText(postFilter.join(' ')); }

int SearchTab::imagesPerPage() const
{ return ui_spinImagesPerPage->value(); }
int SearchTab::columns() const
{ return ui_spinColumns->value(); }
QStringList SearchTab::postFilter(bool includeGlobal) const
{
	QString ret = m_postFiltering->toPlainText();
	if (includeGlobal && !m_settings->value("globalPostFilterExplicit", false).toBool()) {
		QString globalPostFilter = m_settings->value("globalPostFilter").toString();
		if (!globalPostFilter.isEmpty()) {
			ret += " " + globalPostFilter;
		}
	}
	return ret.split(' ', Qt::SkipEmptyParts);
}

const QString &SearchTab::screenName() const
{ return m_screenName; }

bool SearchTab::isLocked() const
{ return m_isLocked; }
void SearchTab::setLocked(bool locked)
{ m_isLocked = locked; }
