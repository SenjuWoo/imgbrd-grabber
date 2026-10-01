#include "viewer/library-source-dialog.h"
#include <QComboBox>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QImage>
#include <QJsonArray>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPixmap>
#include <QPushButton>
#include <QRegularExpression>
#include <QSet>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QtConcurrent>
#include <algorithm>
#include <cmath>
#include "models/image.h"
#include "models/library-importer.h"
#include "models/library-store.h"
#include "models/page.h"
#include "models/profile.h"
#include "models/search-query/search-query.h"
#include "models/site.h"


LibrarySourceDialog::LibrarySourceDialog(Profile *profile, const QString &key, QWidget *parent)
	: QDialog(parent), m_profile(profile), m_store(profile->library()), m_key(key)
{
	setObjectName("librarySourceDialog");
	setWindowTitle(tr("Find and link source"));
	resize(720, 640);
	const auto entry = m_store->entry(key);
	m_local = entry.image.value("local_import").toObject();
	m_thumbnail = entry.thumbnail;
	auto *layout = new QVBoxLayout(this);
	auto *intro = new QLabel(tr("Use metadata or an exact file hash to find the original post. Visual matches are suggestions. Review a candidate before linking it."), this);
	intro->setWordWrap(true);
	layout->addWidget(intro);
	auto *picture = new QLabel(this);
	picture->setAlignment(Qt::AlignCenter);
	QPixmap thumbnail;
	thumbnail.loadFromData(entry.thumbnail);
	picture->setPixmap(thumbnail.scaled(300, 140, Qt::KeepAspectRatio, Qt::SmoothTransformation));
	layout->addWidget(picture);
	auto *name = new QLabel(entry.image.value("name").toString(), this);
	name->setTextFormat(Qt::PlainText);
	name->setWordWrap(true);
	layout->addWidget(name);
	auto *urls = new QListWidget(this);
	urls->setObjectName("librarySourceUrls");
	urls->setMaximumHeight(90);
	QSet<QString> seenUrls;
	for (const auto &value : m_local.value("source_urls").toArray()) {
		const QUrl url(value.toString(), QUrl::StrictMode);
		const QString text = url.toString();
		if (isSourceUrl(url) && !seenUrls.contains(text)) {
			auto *item = new QListWidgetItem(text, urls);
			item->setData(Qt::UserRole, url);
			seenUrls.insert(text);
		}
	}
	if (urls->count() > 0) {
		layout->addWidget(new QLabel(tr("URLs from metadata (unverified; double-click to open):"), this));
		layout->addWidget(urls);
	} else {
		urls->hide();
	}
	connect(urls, &QListWidget::itemActivated, this, [](QListWidgetItem *item) {
		const QUrl url = item->data(Qt::UserRole).toUrl();
		if (isSourceUrl(url)) {
			QDesktopServices::openUrl(url);
		}
	});
	auto *lookupRow = new QHBoxLayout();
	m_source = new QComboBox(this);
	m_source->setObjectName("librarySourceSite");
	m_source->setAccessibleName(tr("Source for exact hash lookup"));
	for (auto *site : profile->getSites()) {
		if (!site->getApis().isEmpty()) {
			m_source->addItem(site->name() + " (" + site->url() + ")", site->url());
		}
	}
	m_hash = new QComboBox(this);
	m_hash->setObjectName("librarySourceHash");
	m_hash->setAccessibleName(tr("MD5 hash to search"));
	const QRegularExpression md5Pattern("^[0-9a-fA-F]{32}$");
	for (const QString &field : { QString("source_md5"), QString("md5") }) {
		const QString hash = m_local.value(field).toString().toLower();
		if (md5Pattern.match(hash).hasMatch() && m_hash->findData(hash) < 0) {
			m_hash->addItem((field == "source_md5" ? tr("Metadata MD5: %1") : tr("File MD5: %1")).arg(hash), hash);
		}
	}
	m_lookup = new QPushButton(tr("Find exact match"), this);
	m_lookup->setObjectName("librarySourceExact");
	m_lookup->setEnabled(m_store->isReady() && !entry.key.isEmpty() && m_source->count() > 0 && m_hash->count() > 0);
	lookupRow->addWidget(m_source, 1);
	lookupRow->addWidget(m_hash, 1);
	lookupRow->addWidget(m_lookup);
	layout->addLayout(lookupRow);
	connect(m_lookup, &QPushButton::clicked, this, &LibrarySourceDialog::exactLookup);
	m_similar = new QPushButton(tr("Find similar cached pictures"), this);
	m_similar->setObjectName("librarySourceSimilar");
	m_similar->setToolTip(tr("Compare only with thumbnails already in your Library. No picture is uploaded."));
	m_similar->setEnabled(m_store->isReady() && !entry.key.isEmpty());
	layout->addWidget(m_similar);
	connect(m_similar, &QPushButton::clicked, this, &LibrarySourceDialog::findSimilar);
	m_status = new QLabel(tr("Exact lookup sends only the selected MD5 hash to the selected source. Similarity search stays on this PC."), this);
	m_status->setObjectName("librarySourceStatus");
	m_status->setTextFormat(Qt::PlainText);
	m_status->setWordWrap(true);
	layout->addWidget(m_status);
	m_results = new QListWidget(this);
	m_results->setObjectName("librarySourceCandidates");
	m_results->setIconSize(QSize(100, 80));
	m_results->setAccessibleName(tr("Source candidates"));
	layout->addWidget(m_results, 1);
	connect(m_results, &QListWidget::itemActivated, this, [this](QListWidgetItem *item) {
		const int row = m_results->row(item);
		if (row >= 0 && row < m_candidates.size() && isSourceUrl(m_candidates[row].image->pageUrl())) {
			QDesktopServices::openUrl(m_candidates[row].image->pageUrl());
		}
	});
	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
	m_link = buttons->addButton(tr("Link selected source…"), QDialogButtonBox::AcceptRole);
	m_link->setObjectName("librarySourceLink");
	m_link->setEnabled(false);
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
	connect(m_link, &QPushButton::clicked, this, &LibrarySourceDialog::linkSelected);
	connect(m_results, &QListWidget::currentRowChanged, this, [this](int row) { m_link->setEnabled(row >= 0 && row < m_candidates.size() && m_page.isNull() && !m_cancelVisual); });
	layout->addWidget(buttons);
	m_timeout = new QTimer(this);
	m_timeout->setSingleShot(true);
	connect(m_timeout, &QTimer::timeout, this, [this]() { cancelLookup(); m_status->setText(tr("The lookup timed out. Try another source or use cached visual candidates.")); });
}

LibrarySourceDialog::~LibrarySourceDialog()
{
	cancelLookup();
}

bool LibrarySourceDialog::isSourceUrl(const QUrl &url)
{
	return url.isValid() && !url.host().isEmpty() && url.userInfo().isEmpty() && (url.scheme().compare("https", Qt::CaseInsensitive) == 0 || url.scheme().compare("http", Qt::CaseInsensitive) == 0);
}

void LibrarySourceDialog::clearCandidates()
{
	m_candidates.clear();
	m_results->clear();
	m_link->setEnabled(false);
}

void LibrarySourceDialog::addCandidate(const QSharedPointer<Image> &image, const QByteArray &thumbnail, const QString &label, const QString &evidence)
{
	m_candidates.append({ image, evidence });
	auto *item = new QListWidgetItem(label + "\n" + image->parentSite()->url() + " · " + tr("Picture #%1").arg(image->id()), m_results);
	item->setToolTip(image->pageUrl().toString());
	QPixmap pixels;
	pixels.loadFromData(thumbnail);
	if (!pixels.isNull()) {
		item->setIcon(QIcon(pixels));
	}
}

void LibrarySourceDialog::exactLookup()
{
	if (!m_page.isNull()) {
		cancelLookup();
		m_status->setText(tr("Lookup canceled."));
		return;
	}
	Site *site = m_profile->getSites().value(m_source->currentData().toString());
	const QString hash = m_hash->currentData().toString();
	if (site == nullptr || hash.isEmpty()) {
		return;
	}
	cancelLookup();
	clearCandidates();
	m_source->setEnabled(false);
	m_hash->setEnabled(false);
	m_lookup->setText(tr("Cancel lookup"));
	m_status->setText(tr("Checking the selected source for an exact MD5 match…"));
	m_page = new Page(m_profile, site, { site }, SearchQuery(QStringList { "md5:" + hash }), 1, 20, {}, false, this);
	connect(m_page, &Page::finishedLoading, this, [this, hash](Page *page) { showExactResults(page, hash); });
	connect(m_page, &Page::failedLoading, this, [this](Page *page) {
		const QString reason = page->errors().join("\n");
		cancelLookup();
		m_status->setText(reason.isEmpty() ? tr("Exact lookup failed. Try another source.") : reason);
	});
	m_timeout->start(30000);
	m_page->load();
}

void LibrarySourceDialog::cancelLookup()
{
	m_timeout->stop();
	if (m_cancelVisual) {
		m_cancelVisual->store(true);
		m_cancelVisual.reset();
	}
	++m_visualGeneration;
	m_similar->setText(tr("Find similar cached pictures"));
	if (!m_page.isNull()) {
		disconnect(m_page, nullptr, this, nullptr);
		m_page->abort();
		m_page->deleteLater();
		m_page.clear();
	}
	m_source->setEnabled(true);
	m_hash->setEnabled(true);
	m_lookup->setText(tr("Find exact match"));
	const int row = m_results->currentRow();
	m_link->setEnabled(row >= 0 && row < m_candidates.size());
}

void LibrarySourceDialog::showExactResults(Page *page, const QString &hash)
{
	for (const auto &image : page->images()) {
		if (image->md5().compare(hash, Qt::CaseInsensitive) == 0) {
			// Keep a standalone metadata image after the search Page is discarded.
			QJsonObject metadata;
			image->write(metadata);
			auto candidate = QSharedPointer<Image>::create(m_profile);
			if (candidate->read(metadata, m_profile->getSites())) {
				addCandidate(candidate, m_store->entry(m_store->keyForImage(*candidate)).thumbnail, tr("Exact MD5 match — review before linking"), "User confirmed exact-md5:" + hash);
			}
		}
	}
	cancelLookup();
	m_status->setText(m_candidates.isEmpty() ? tr("No returned picture had the selected MD5. No source was linked.") : tr("The source reports this exact MD5. Double-click a candidate to review its post, then confirm the link."));
}

void LibrarySourceDialog::findSimilar()
{
	if (m_cancelVisual) {
		cancelLookup();
		m_status->setText(tr("Visual comparison canceled."));
		return;
	}
	cancelLookup();
	clearCandidates();
	QString targetHash = m_local.value("visual_hash").toString();
	if (LibraryImporter::visualDistance(targetHash, targetHash) < 0) {
		targetHash = LibraryImporter::visualHash(QImage::fromData(m_thumbnail));
	}
	if (targetHash.isEmpty()) {
		m_status->setText(tr("This picture has no usable thumbnail for visual comparison."));
		return;
	}
	const QSize targetSize(m_local.value("width").toInt(), m_local.value("height").toInt());
	const double targetAspect = targetSize.height() > 0 ? static_cast<double>(targetSize.width()) / targetSize.height() : 0;
	struct Match { QJsonObject metadata; QByteArray thumbnail; int distance; };
	const auto entries = m_store->entries(); // SQLite stays on its owning thread; only pixels and values enter the worker.
	const auto sites = m_profile->getSites().keys();
	const auto token = std::make_shared<std::atomic_bool>(false);
	m_cancelVisual = token;
	const int generation = m_visualGeneration;
	m_similar->setText(tr("Cancel comparison"));
	m_status->setText(tr("Comparing cached Library thumbnails on this PC…"));
	auto *watcher = new QFutureWatcher<QList<Match>>(this);
	connect(watcher, &QFutureWatcher<QList<Match>>::finished, this, [this, watcher, generation]() {
		const auto matches = watcher->result();
		watcher->deleteLater();
		if (generation != m_visualGeneration) {
			return;
		}
		m_cancelVisual.reset();
		m_similar->setText(tr("Find similar cached pictures"));
		for (const auto &match : matches) {
			auto image = QSharedPointer<Image>::create(m_profile);
			if (image->read(match.metadata, m_profile->getSites())) {
				addCandidate(image, match.thumbnail, tr("Visual candidate · %1/64 hash bits differ").arg(match.distance), "User confirmed visual-candidate:" + QString::number(match.distance) + "/64");
			}
		}
		m_status->setText(m_candidates.isEmpty() ? tr("No similar source pictures were found in cached Library thumbnails.") : tr("These are visual suggestions, not verified matches. Low-detail pictures can share a hash. Double-click to review the source post before linking."));
	});
	watcher->setFuture(QtConcurrent::run([entries, sites, token, targetHash, targetAspect, key = m_key]() {
		QList<Match> matches;
		for (const auto &entry : entries) {
			if (token->load()) {
				return QList<Match>();
			}
			if (entry.key == key || !sites.contains(entry.image.value("website").toString())) {
				continue;
			}
			const QImage thumbnail = QImage::fromData(entry.thumbnail);
			if (thumbnail.isNull()) {
				continue;
			}
			const double aspect = static_cast<double>(thumbnail.width()) / thumbnail.height();
			if (targetAspect > 0 && std::abs(targetAspect - aspect) / std::max(targetAspect, aspect) > 0.2) {
				continue;
			}
			const int distance = LibraryImporter::visualDistance(targetHash, LibraryImporter::visualHash(thumbnail));
			if (distance >= 0 && distance <= 12) {
				matches.append({ entry.image, entry.thumbnail, distance });
			}
		}
		std::sort(matches.begin(), matches.end(), [](const Match &left, const Match &right) { return left.distance < right.distance; });
		if (matches.size() > 30) {
			matches = matches.mid(0, 30);
		}
		return matches;
	}));
}

void LibrarySourceDialog::linkSelected()
{
	const int row = m_results->currentRow();
	if (row < 0 || row >= m_candidates.size()) {
		return;
	}
	const auto candidate = m_candidates[row];
	const QString text = tr("Link this picture to %1, picture #%2?\n\nYour local file stays on disk. Library preferences and collection membership are preserved.\n\n%3").arg(candidate.image->parentSite()->url()).arg(candidate.image->id()).arg(candidate.evidence.contains("visual-candidate:") ? tr("This visual suggestion has not been verified as an exact match.") : tr("The source reports the selected MD5 hash."));
	if (QMessageBox::question(this, tr("Confirm source link"), text, QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) {
		return;
	}
	if (!m_store->linkSource(m_key, *candidate.image, candidate.evidence)) {
		QMessageBox::warning(this, tr("Source link"), m_store->lastError());
		return;
	}
	accept();
}
