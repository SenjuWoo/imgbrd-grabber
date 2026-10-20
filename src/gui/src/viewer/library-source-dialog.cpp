#include "viewer/library-source-dialog.h"
#include <QComboBox>
#include <QCryptographicHash>
#include <QFileInfo>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QClipboard>
#include <QGuiApplication>
#include <QImage>
#include <QImageReader>
#include <QMenu>
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
#include "logger.h"
#include "models/profile.h"
#include "models/search-query/search-query.h"
#include "models/site.h"
#include "network/network-reply.h"
#include "tags/tag.h"


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
	if (!thumbnail.isNull()) {
		picture->setPixmap(thumbnail.scaled(300, 140, Qt::KeepAspectRatio, Qt::SmoothTransformation));
	}
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
	m_source->addItem(tr("All my sources (one after another)"), QStringLiteral("*"));
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
	// Pixiv names its files "<artwork>_p<page>"; the artwork's public page then gives the original file to compare.
	QStringList names = entry.localPaths;
	for (const auto &value : m_local.value("source_urls").toArray()) {
		names.append(value.toString());
	}
	names.append(entry.image.value("name").toString());
	for (const QString &name : names) {
		const auto id = PixivArtwork::idFrom(name.contains("pixiv") ? name : QFileInfo(name).fileName());
		if (!id.first.isEmpty()) {
			m_pixivId = id;
			break;
		}
	}
	m_pixiv = new QPushButton(tr("Find on Pixiv (artwork #%1, page %2)").arg(m_pixivId.first).arg(m_pixivId.second + 1), this);
	m_pixiv->setObjectName("librarySourcePixiv");
	m_pixiv->setToolTip(tr("Reads the artwork's public Pixiv page and compares the original file with yours. No login needed; nothing is uploaded."));
	m_pixiv->setVisible(!m_pixivId.first.isEmpty());
	m_pixiv->setEnabled(m_store->isReady() && !entry.key.isEmpty());
	layout->addWidget(m_pixiv);
	connect(m_pixiv, &QPushButton::clicked, this, &LibrarySourceDialog::findOnPixiv);
	// Reverse image sites block automated clients, so the app never uploads: it copies the picture and opens the site in the browser.
	for (const QString &path : entry.localPaths) {
		if (QFileInfo::exists(path)) {
			m_localFile = path;
			break;
		}
	}
	m_reverse = new QPushButton(tr("Reverse image search…"), this);
	m_reverse->setObjectName("librarySourceReverse");
	m_reverse->setToolTip(tr("For pictures no booru has by MD5. Copies the picture to the clipboard and opens the site in your browser: press Ctrl+V there. You choose what to upload; Grabber sends nothing."));
	m_reverse->setEnabled(!m_localFile.isEmpty());
	auto *reverseMenu = new QMenu(m_reverse);
	const QList<QPair<QString, QString>> engines {
		{ QStringLiteral("SauceNAO"), QStringLiteral("https://saucenao.com/") }, { QStringLiteral("IQDB"), QStringLiteral("https://iqdb.org/") },
		{ QStringLiteral("Google Lens"), QStringLiteral("https://lens.google.com/") }, { QStringLiteral("Yandex Images"), QStringLiteral("https://yandex.com/images/") },
		{ QStringLiteral("TinEye"), QStringLiteral("https://tineye.com/") },
	};
	for (const auto &engine : engines) {
		reverseMenu->addAction(engine.first, this, [this, url = QUrl(engine.second)]() { reverseSearch(url); });
	}
	reverseMenu->addSeparator();
	reverseMenu->addAction(tr("Show the file in its folder"), this, [this]() { QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(m_localFile).absolutePath())); });
	m_reverse->setMenu(reverseMenu);
	layout->addWidget(m_reverse);
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
	connect(m_timeout, &QTimer::timeout, this, [this]() {
		if (!m_page.isNull()) {
			log(QStringLiteral("Source lookup on %1 timed out").arg(m_page->site()->url()), Logger::Warning);
			lookupNext(m_hash->currentData().toString()); // A slow source does not hold up the others.
		}
	});
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
	const QString hash = m_hash->currentData().toString();
	if (hash.isEmpty()) {
		return;
	}
	// Grabber most likely saved the file from one of the selected sources: ask each of them in turn.
	QStringList sites;
	const auto &known = m_profile->getSites();
	const QStringList wanted = m_source->currentData().toString() == QLatin1String("*")
		? m_profile->getSettings()->value("sites").toStringList()
		: QStringList { m_source->currentData().toString() };
	for (const QString &url : wanted) {
		if (known.contains(url) && !known.value(url)->getApis().isEmpty()) {
			sites.append(url);
		}
	}
	if (sites.isEmpty()) {
		m_status->setText(tr("Choose image sources in a search tab first."));
		return;
	}
	cancelLookup();
	clearCandidates();
	m_source->setEnabled(false);
	m_hash->setEnabled(false);
	m_lookup->setText(tr("Cancel lookup"));
	m_lookupSites = sites;
	m_lookupTotal = int(sites.size());
	lookupNext(hash);
}

void LibrarySourceDialog::lookupNext(const QString &hash)
{
	if (!m_page.isNull()) {
		disconnect(m_page, nullptr, this, nullptr);
		m_page->abort();
		m_page->deleteLater();
		m_page.clear();
	}
	if (m_lookupSites.isEmpty()) {
		cancelLookup();
		m_status->setText(!m_candidates.isEmpty() ? tr("The source reports this exact MD5. Double-click a candidate to review its post, then confirm the link.")
			: tr("None of %n source(s) has a picture with this MD5: it was deleted there or came from another site. A reverse image search (SauceNAO, IQDB) can still find it.", "", m_lookupTotal));
		return;
	}
	Site *site = m_profile->getSites().value(m_lookupSites.takeFirst());
	m_status->setText(tr("Checking %1 for an exact MD5 match (%2 of %3)…").arg(site->name()).arg(m_lookupTotal - m_lookupSites.size()).arg(m_lookupTotal));
	m_page = new Page(m_profile, site, { site }, SearchQuery(QStringList { "md5:" + hash }), 1, 20, {}, false, this);
	connect(m_page, &Page::finishedLoading, this, [this, hash](Page *page) { showExactResults(page, hash); });
	connect(m_page, &Page::failedLoading, this, [this, hash](Page *page) {
		log(QStringLiteral("Source lookup on %1 failed: %2").arg(page->site()->url(), page->errors().join("; ")), Logger::Warning);
		lookupNext(hash);
	});
	m_timeout->start(30000);
	m_page->load();
}

void LibrarySourceDialog::cancelLookup()
{
	m_timeout->stop();
	m_lookupSites.clear();
	if (m_pixivReply) {
		disconnect(m_pixivReply, nullptr, this, nullptr);
		m_pixivReply->abort();
		m_pixivReply->deleteLater();
		m_pixivReply.clear();
	}
	m_pixiv->setEnabled(true);
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
				// Same file: the key may resolve to this very picture, whose imported pixels must not pose as the source's.
				const QString known = m_store->keyForImage(*candidate);
				addCandidate(candidate, known == m_key ? QByteArray() : m_store->entry(known).thumbnail, tr("Exact MD5 match — review before linking"), "User confirmed exact-md5:" + hash);
			}
		}
	}
	lookupNext(hash);
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

void LibrarySourceDialog::reverseSearch(const QUrl &site)
{
	QImageReader reader(m_localFile);
	reader.setAutoTransform(true);
	const QImage image = reader.read();
	if (image.isNull()) {
		m_status->setText(tr("Could not read %1 to copy it: %2").arg(QFileInfo(m_localFile).fileName(), reader.errorString()));
		return;
	}
	QGuiApplication::clipboard()->setImage(image);
	QDesktopServices::openUrl(site);
	m_status->setText(tr("Picture copied. On %1 press Ctrl+V, or drag %2 onto the page. Found the post? Search its number here or add the site as a source.").arg(site.host(), QFileInfo(m_localFile).fileName()));
}

void LibrarySourceDialog::findOnPixiv()
{
	Site *site = m_profile->getSites().value("www.pixiv.net");
	if (site == nullptr) {
		m_status->setText(tr("Add the Pixiv source (www.pixiv.net) to your sources first."));
		return;
	}
	cancelLookup();
	clearCandidates();
	m_pixiv->setEnabled(false);
	m_status->setText(tr("Asking Pixiv about artwork #%1…").arg(m_pixivId.first));
	PixivArtwork artwork;
	artwork.id = m_pixivId.first;
	m_pixivReply = site->get(artwork.detailsUrl(), Site::QueryType::Details, artwork.pageUrl(), "page");
	connect(m_pixivReply, &NetworkReply::finished, this, [this, reply = m_pixivReply.data()]() { pixivDetailsLoaded(reply); });
	m_timeout->start(30000);
}

void LibrarySourceDialog::pixivDetailsLoaded(NetworkReply *reply)
{
	if (reply != m_pixivReply) {
		return;
	}
	m_pixivReply.clear();
	reply->deleteLater();
	QString error;
	m_artwork = reply->error() == NetworkReply::NetworkError::NoError
		? PixivArtwork::fromDetails(reply->readAll(), m_pixivId.first, m_pixivId.second, &error)
		: PixivArtwork();
	if (!m_artwork.isValid()) {
		cancelLookup();
		m_status->setText(tr("Pixiv lookup failed: %1").arg(error.isEmpty() ? reply->errorString() : error));
		return;
	}
	m_pixivFiles = m_artwork.originalCandidates();
	tryPixivFile();
}

void LibrarySourceDialog::tryPixivFile()
{
	Site *site = m_profile->getSites().value("www.pixiv.net");
	if (site == nullptr || m_pixivFiles.isEmpty()) {
		cancelLookup();
		addPixivCandidate(m_artwork.sampleUrl(), {}, false);
		m_status->setText(tr("Found the Pixiv artwork, but could not download its original file to compare. Review it before linking."));
		return;
	}
	m_status->setText(tr("Comparing your file with Pixiv's original…"));
	const QUrl url = m_pixivFiles.takeFirst();
	m_pixivReply = site->get(url, Site::QueryType::Img, m_artwork.pageUrl(), "image");
	connect(m_pixivReply, &NetworkReply::finished, this, [this, url, reply = m_pixivReply.data()]() {
		if (reply != m_pixivReply) {
			return;
		}
		m_pixivReply.clear();
		reply->deleteLater();
		if (reply->error() != NetworkReply::NetworkError::NoError) {
			tryPixivFile(); // Wrong extension guess: try the next one.
			return;
		}
		const QString md5 = QString::fromLatin1(QCryptographicHash::hash(reply->readAll(), QCryptographicHash::Md5).toHex());
		const QString local = m_local.value("md5").toString().toLower();
		cancelLookup();
		addPixivCandidate(url, md5, md5 == local);
		m_status->setText(md5 == local ? tr("Pixiv's original file is byte-for-byte your file. Review it, then link it.")
			: tr("Found the Pixiv artwork, but its original file differs from yours (edited or re-saved). Review it before linking."));
	});
}

void LibrarySourceDialog::addPixivCandidate(const QUrl &file, const QString &md5, bool exact)
{
	Site *site = m_profile->getSites().value("www.pixiv.net");
	if (site == nullptr) {
		return;
	}
	QString name = m_artwork.title;
	if (m_artwork.pageCount > 1) {
		name += ' ' + tr("(page %1 of %2)").arg(m_artwork.page + 1).arg(m_artwork.pageCount);
	}
	QMap<QString, QString> details {
		{"id", m_artwork.id}, {"file_url", file.toString()}, {"sample_url", m_artwork.sampleUrl().toString()},
		{"preview_url", m_artwork.thumbnailUrl().toString()}, {"page_url", m_artwork.pageUrl().toString()}, {"name", name},
	};
	if (!md5.isEmpty()) {
		details.insert("md5", md5);
	}
	if (m_artwork.page == 0 && m_artwork.firstPageSize.isValid()) {
		details.insert("width", QString::number(m_artwork.firstPageSize.width()));
		details.insert("height", QString::number(m_artwork.firstPageSize.height()));
	}
	QList<Tag> tags;
	if (!m_artwork.author.isEmpty()) {
		tags.append(Tag(QString(m_artwork.author).replace(' ', '_'), "artist"));
	}
	for (const QString &tag : m_artwork.tags) {
		tags.append(Tag(tag, "general"));
	}
	// Each page of an artwork is its own Library picture.
	const QVariantMap identity {{"id", m_artwork.id}, {"page", m_artwork.page}};
	auto image = QSharedPointer<Image>::create(site, details, identity, QVariantMap {{"tags", QVariant::fromValue(tags)}}, m_profile);
	addCandidate(image, m_thumbnail, exact ? tr("Exact file match on Pixiv — review before linking") : tr("Same Pixiv artwork, file not identical — review before linking"),
		exact ? "User confirmed exact-md5:" + md5 : "User confirmed pixiv-artwork-from-file-name:" + m_artwork.id + "_p" + QString::number(m_artwork.page));
}

void LibrarySourceDialog::linkSelected()
{
	const int row = m_results->currentRow();
	if (row < 0 || row >= m_candidates.size()) {
		return;
	}
	const auto candidate = m_candidates[row];
	const QString evidence = candidate.evidence.contains("visual-candidate:") ? tr("This visual suggestion has not been verified as an exact match.")
		: candidate.evidence.contains("pixiv-artwork-from-file-name:") ? tr("The artwork ID comes from the file name; the files are not identical.")
		: tr("The source reports the selected MD5 hash.");
	const QString text = tr("Link this picture to %1, picture #%2?\n\nYour local file stays on disk. Library preferences and collection membership are preserved.\n\n%3").arg(candidate.image->parentSite()->url()).arg(candidate.image->id()).arg(evidence);
	if (QMessageBox::question(this, tr("Confirm source link"), text, QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) {
		return;
	}
	if (!m_store->linkSource(m_key, *candidate.image, candidate.evidence)) {
		QMessageBox::warning(this, tr("Source link"), m_store->lastError());
		return;
	}
	accept();
}
