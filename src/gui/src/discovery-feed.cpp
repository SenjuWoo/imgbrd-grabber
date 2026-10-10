#include "discovery-feed.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QSettings>
#include <QTimer>
#include <algorithm>
#include <cmath>
#include "models/image.h"
#include "models/library-image-encoder.h"
#include "models/library-recommendations.h"
#include "models/library-store.h"
#include "models/page.h"
#include "models/profile.h"
#include "models/site.h"
#include "models/visual-encoder.h"
#include "logger.h"
#include "thumbnail-loader.h"
#include "utils/file-utils.h"


namespace
{
	constexpr int BatchQueries = 4;
	constexpr int PageLimit = 30;
	constexpr int ConcurrentPreviews = 8;
	constexpr int MaxSeen = 4000;
	constexpr int MaxHidden = 5000;
	constexpr int MaxFeedback = 3000;
	// Newest posts sampled per batch to learn how common tags are on each source; only the few that fit the taste are shown.
	constexpr int SampleLimit = 100;
	constexpr int SampleShown = 6;
	constexpr double WellSampled = 200;

	void zNormalize(QVector<double> &values)
	{
		if (values.isEmpty()) {
			return;
		}
		double mean = 0;
		for (const double value : values) {
			mean += value;
		}
		mean /= values.size();
		double variance = 0;
		for (const double value : values) {
			variance += (value - mean) * (value - mean);
		}
		const double deviation = std::sqrt(variance / values.size());
		for (double &value : values) {
			value = deviation > 1e-9 ? (value - mean) / deviation : 0;
		}
	}
}

QString DiscoveryFeed::statePath(const QString &profileDirectory)
{
	return QDir(profileDirectory).filePath("discover.json");
}

DiscoveryFeed::DiscoveryFeed(Profile *profile, LibraryRecommendations *recommendations, QObject *parent)
	: QObject(parent), m_profile(profile), m_recommendations(recommendations), m_timeout(new QTimer(this))
{
	m_seed = QRandomGenerator::global()->generate();
	m_timeout->setSingleShot(true);
	connect(m_timeout, &QTimer::timeout, this, &DiscoveryFeed::finishBatch);
	connect(profile->library(), &LibraryStore::imageChanged, this, &DiscoveryFeed::ratingChanged);
	connect(recommendations, &LibraryRecommendations::changed, this, [this]() { m_tasteDirty = true; });
	loadState();
}

DiscoveryFeed::~DiscoveryFeed()
{
	cancel();
	saveState();
}

void DiscoveryFeed::ratingChanged()
{
	m_tasteDirty = true;
	// Credit the search that found a picture once it is liked or favorited; Library keys can differ through source links.
	for (auto it = m_shownQueries.constBegin(); it != m_shownQueries.constEnd(); ++it) {
		const auto image = m_shown.value(it.key());
		if (image && !m_credited.contains(it.key()) && m_profile->library()->isRated(*image)) {
			m_credited.insert(it.key());
			for (const QString &tag : it.value()) {
				m_feedback[tag].rated += 1;
			}
		}
	}
}

void DiscoveryFeed::loadState()
{
	QFile file(statePath(m_profile->getPath()));
	if (!file.open(QIODevice::ReadOnly) || file.size() > 8 * 1024 * 1024) {
		return;
	}
	const auto root = QJsonDocument::fromJson(file.readAll()).object();
	for (const auto &value : root.value("seen").toArray()) {
		m_seen.append(value.toString());
	}
	for (const auto &value : root.value("hidden").toArray()) {
		m_hidden.append(value.toString());
	}
	m_seen = m_seen.mid(std::max<qsizetype>(0, m_seen.size() - MaxSeen));
	m_hidden = m_hidden.mid(std::max<qsizetype>(0, m_hidden.size() - MaxHidden));
	m_seenSet = QSet<QString>(m_seen.begin(), m_seen.end());
	m_hiddenSet = QSet<QString>(m_hidden.begin(), m_hidden.end());
	const auto disliked = root.value("disliked").toObject();
	for (auto it = disliked.constBegin(); it != disliked.constEnd(); ++it) {
		if (it.value().toDouble() > 0) {
			m_dislikedTags.insert(it.key(), it.value().toDouble());
		}
	}
	m_background = TagBackground::fromJson(root.value("background").toObject());
	const auto feedback = root.value("feedback").toObject();
	for (auto it = feedback.constBegin(); it != feedback.constEnd() && m_feedback.size() < MaxFeedback; ++it) {
		const auto values = it.value().toArray();
		const double shown = values.at(0).toDouble(), rated = values.at(1).toDouble();
		if (shown >= 0 && rated >= 0) {
			m_feedback.insert(it.key(), {shown, rated});
		}
	}
}

void DiscoveryFeed::saveState() const
{
	QJsonObject disliked;
	for (auto it = m_dislikedTags.constBegin(); it != m_dislikedTags.constEnd(); ++it) {
		disliked.insert(it.key(), it.value());
	}
	// Keep the most-shown searches; the rest carry too little evidence to matter.
	QList<QPair<double, QString>> shown;
	for (auto it = m_feedback.constBegin(); it != m_feedback.constEnd(); ++it) {
		shown.append({it->shown + it->rated, it.key()});
	}
	std::sort(shown.begin(), shown.end(), [](const auto &left, const auto &right) { return left.first > right.first; });
	QJsonObject feedback;
	for (const auto &entry : shown.mid(0, MaxFeedback)) {
		const auto &value = m_feedback[entry.second];
		feedback.insert(entry.second, QJsonArray {value.shown, value.rated});
	}
	const QJsonObject root {
		{"version", 2},
		{"seen", QJsonArray::fromStringList(m_seen)},
		{"hidden", QJsonArray::fromStringList(m_hidden)},
		{"disliked", disliked},
		{"background", m_background.toJson()},
		{"feedback", feedback},
	};
	const QString path = statePath(m_profile->getPath());
	if (ensureFileParent(path)) {
		safeWriteFile(path, QJsonDocument(root).toJson(QJsonDocument::Compact));
	}
}

void DiscoveryFeed::setScope(qint64 collection)
{
	if (collection != m_scope) {
		m_scope = collection;
		m_tasteDirty = true;
	}
}

qint64 DiscoveryFeed::scope() const { return m_scope; }
bool DiscoveryFeed::isBusy() const { return m_busy; }
int DiscoveryFeed::dismissedCount() const { return int(m_hidden.size()); }

void DiscoveryFeed::setVisualEnabled(bool enabled)
{
	m_visualEnabled = enabled;
	const QString model = LibraryImageEncoder::modelPath(m_profile->getPath());
	const bool usable = enabled && QFileInfo(model).size() == LibraryImageEncoder::modelSize();
	if (usable && m_encoder == nullptr) {
		m_encoder = new VisualEncoder(model, {}, this);
		connect(m_encoder, &VisualEncoder::encoded, this, &DiscoveryFeed::encoded);
		connect(m_encoder, &VisualEncoder::failed, this, [this](const QString &key) { candidateDone(key); });
	} else if (!usable && m_encoder != nullptr) {
		delete m_encoder;
		m_encoder = nullptr;
	}
	m_tasteDirty = true;
}

bool DiscoveryFeed::visualActive() const
{
	return m_encoder != nullptr && !m_visualSeeds.isEmpty();
}

const TasteProfile &DiscoveryFeed::taste()
{
	if (m_tasteDirty) {
		rebuildTaste();
	}
	return m_taste;
}

void DiscoveryFeed::rebuildTaste()
{
	m_tasteDirty = false;
	m_taste = TasteProfile::build(m_profile->library()->entries(m_scope), m_dislikedTags, QDateTime::currentDateTimeUtc(), m_background);
	m_visualSeeds.clear();
	if (m_encoder != nullptr) {
		const auto &vectors = m_recommendations->vectors();
		for (auto it = m_taste.seedWeights().constBegin(); it != m_taste.seedWeights().constEnd(); ++it) {
			const auto vector = vectors.value(it.key());
			if (vector.size() == 512) {
				m_visualSeeds.append({vector, it.value()});
			}
		}
	}
}

QSharedPointer<Image> DiscoveryFeed::image(const QString &key) const
{
	return m_shown.value(key);
}

void DiscoveryFeed::dismiss(const QString &key)
{
	if (!m_hiddenSet.contains(key)) {
		m_hidden.append(key);
		m_hiddenSet.insert(key);
		while (m_hidden.size() > MaxHidden) {
			m_hiddenSet.remove(m_hidden.takeFirst());
		}
	}
	if (const auto image = m_shown.value(key)) {
		for (const QString &tag : image->tagsString()) {
			m_dislikedTags[TasteProfile::normalize(tag)] += 1;
		}
	}
	// Hiding is stronger than scrolling past.
	for (const QString &tag : m_shownQueries.value(key)) {
		m_feedback[tag].shown += 2;
	}
	m_tasteDirty = true;
	saveState();
}

void DiscoveryFeed::resetDismissed()
{
	m_hidden.clear();
	m_hiddenSet.clear();
	m_dislikedTags.clear();
	m_tasteDirty = true;
	saveState();
}

void DiscoveryFeed::setBusy(bool busy)
{
	if (busy != m_busy) {
		m_busy = busy;
		emit busyChanged(busy);
	}
}

void DiscoveryFeed::cancel()
{
	m_timeout->stop();
	const auto pages = m_pages.keys();
	m_pages.clear();
	for (auto *page : pages) {
		disconnect(page, nullptr, this, nullptr);
		page->abort();
		page->abortTags();
		page->deleteLater();
	}
	for (const auto &loader : m_loaders) {
		if (loader) {
			disconnect(loader, nullptr, this, nullptr);
			loader->abort();
			loader->deleteLater();
		}
	}
	m_loaders.clear();
	m_previewQueue.clear();
	m_candidates.clear();
	m_groups.clear();
	setBusy(false);
}

void DiscoveryFeed::restart()
{
	cancel();
	m_seed = QRandomGenerator::global()->generate();
	m_batch = 0;
	m_emptyBatches = 0;
	// Newest-post samples keep paging so a refresh does not count the same posts twice.
	for (auto it = m_nextPage.begin(); it != m_nextPage.end();) {
		it = it.key().endsWith('|') ? std::next(it) : m_nextPage.erase(it);
	}
	m_exhausted.clear();
	m_shown.clear();
	m_shownQueries.clear();
	m_shownFingerprints.clear();
	m_shownMd5.clear();
	m_tasteDirty = true;
}

void DiscoveryFeed::fetchMore()
{
	if (m_busy) {
		return;
	}
	if (m_tasteDirty) {
		rebuildTaste();
	}
	QStringList sources;
	const auto &sites = m_profile->getSites();
	for (const QString &source : m_profile->getSettings()->value("sites").toStringList()) {
		if (sites.contains(source)) {
			sources.append(source);
		}
	}
	if (sources.isEmpty()) {
		emit statusChanged(tr("Choose image sources in a search tab, then come back to Discover."));
		return;
	}

	const quint32 batchSeed = m_seed + quint32(m_batch) * 7919u;
	QRandomGenerator rng(batchSeed);
	const int wanted = m_taste.isEmpty() ? std::min<int>(BatchQueries, int(sources.size())) : BatchQueries;
	// Sample newest posts until every rated source is well known, then now and then to follow trends.
	QStringList sampleSources;
	bool unsampled = false;
	for (const QString &source : sources) {
		// Only sources with ratings matter for how distinctive a rated tag is.
		if (!m_unsampleable.contains(source) && m_taste.siteWeights().value(source) > 0) {
			sampleSources.append(source);
			unsampled = unsampled || m_background.posts(source) < WellSampled;
		}
	}
	const int samples = m_taste.isEmpty() || sampleSources.isEmpty() ? 0 : (unsampled ? 2 : (m_batch % 3 == 2 ? 1 : 0));
	++m_batch;
	m_failedSources.clear();
	m_candidates.clear();
	m_groups.clear();
	m_previewQueue.clear();
	m_batchEmitted = 0;
	QList<DiscoveryQuery> queries;
	// Sources with the most ratings and the fewest samples first.
	std::sort(sampleSources.begin(), sampleSources.end(), [this](const QString &left, const QString &right) {
		const auto &weights = m_taste.siteWeights();
		return (m_background.posts(left) + 1) / (weights.value(left) + 0.5) < (m_background.posts(right) + 1) / (weights.value(right) + 0.5);
	});
	for (int i = 0; i < samples; ++i) {
		queries.append({sampleSources[i % sampleSources.size()], {}, tr("New on %1").arg(sampleSources[i % sampleSources.size()])});
	}
	queries.append(m_taste.queries(sources, wanted + 3, batchSeed, m_feedback));
	for (const auto &query : queries) {
		if (m_pages.size() >= wanted + samples) {
			break;
		}
		const QString signature = query.website + '|' + query.tags.join(' ');
		Site *site = sites.value(query.website);
		if (site == nullptr || m_exhausted.contains(signature)) {
			continue;
		}
		// Single tags start at a random early page so refreshes are not always the same posts.
		const int page = m_nextPage.value(signature, query.tags.size() == 1 ? 1 + int(rng.bounded(3u)) : 1);
		m_nextPage.insert(signature, page + 1);
		m_queryReasons.insert(signature, query.reason);
		const bool sample = query.tags.isEmpty() && !m_taste.isEmpty();
		log(QStringLiteral("Discover: %1 on %2 (page %3)").arg(query.tags.isEmpty() ? QStringLiteral("newest posts") : query.tags.join(' '), query.website).arg(page), Logger::Info);
		auto *request = new Page(m_profile, site, sites.values(), query.tags, page, sample ? SampleLimit : PageLimit, {}, false, this);
		m_pages.insert(request, signature);
		connect(request, &Page::finishedLoading, this, [this](Page *result) { pageFinished(result, true); });
		connect(request, &Page::failedLoading, this, [this](Page *result) { pageFinished(result, false); });
		connect(request, &Page::httpsRedirect, this, [this](Page *result) { pageFinished(result, false); });
	}
	if (m_pages.isEmpty()) {
		emit statusChanged(tr("You've reached the end of these topics. Refresh for new ones."));
		return;
	}
	setBusy(true);
	emit statusChanged(tr("Finding pictures you'll like…"));
	m_timeout->start(25000);
	// Requests can fail synchronously from cache; register the whole batch before loading.
	for (auto *page : m_pages.keys()) {
		page->load();
	}
}

void DiscoveryFeed::pageFinished(Page *page, bool success)
{
	if (!m_pages.contains(page)) {
		return;
	}
	const QString signature = m_pages.take(page);
	const QString website = signature.section('|', 0, 0);
	const QStringList queryTags = signature.section('|', 1).split(' ', Qt::SkipEmptyParts);
	disconnect(page, nullptr, this, nullptr);
	QList<Candidate> found;
	if (!success) {
		const QString name = page->site()->name();
		if (!m_failedSources.contains(name)) {
			m_failedSources.append(name);
		}
		if (queryTags.isEmpty()) {
			m_unsampleable.insert(website); // Do not spend every batch on a source that refuses anonymous listings.
		}
	} else {
		const auto &images = page->images();
		if (images.isEmpty() || !page->hasNext()) {
			m_exhausted.insert(signature);
		}
		if (queryTags.isEmpty()) {
			// Newest posts are an unbiased sample of the source, so count every one of them.
			for (const auto &source : images) {
				if (source && source->isValid()) {
					m_background.add(website, source->tagsString());
					m_backgroundChanged = true;
				}
			}
		}
		auto *store = m_profile->library();
		QSet<QString> batchMd5;
		for (const auto &candidate : m_candidates) {
			batchMd5.insert(candidate.item.image->md5().toLower());
		}
		for (const auto &source : images) {
			if (!source || !source->isValid() || source->isGallery()
				|| !m_profile->getBlacklist().match(source->tokens(m_profile)).isEmpty()) {
				continue;
			}
			// Viewers can outlive this batch; serialization drops the Page ownership.
			QJsonObject json;
			source->write(json);
			auto image = QSharedPointer<Image>::create(m_profile);
			if (!image->read(json, m_profile->getSites())) {
				continue;
			}
			const QString key = LibraryStore::imageKey(*image);
			const QString md5 = image->md5().toLower();
			if (key.isEmpty() || m_seenSet.contains(key) || m_hiddenSet.contains(key) || m_shown.contains(key) || m_candidates.contains(key)
				|| (!md5.isEmpty() && (m_shownMd5.contains(md5) || batchMd5.contains(md5))) || store->isRated(*image)) {
				continue;
			}
			if (!md5.isEmpty()) {
				batchMd5.insert(md5);
			}
			Candidate candidate;
			candidate.item = {key, image, m_queryReasons.value(signature), 0, false};
			candidate.query = queryTags;
			candidate.tagScore = m_taste.score(image->tagsString());
			found.append(candidate);
		}
	}
	page->deleteLater();

	// Each source streams in on its own; only its most relevant share gets a preview.
	std::stable_sort(found.begin(), found.end(), [](const Candidate &left, const Candidate &right) { return left.tagScore > right.tagScore; });
	if (!m_taste.isEmpty() && queryTags.isEmpty()) {
		found.erase(std::remove_if(found.begin(), found.end(), [](const Candidate &candidate) { return candidate.tagScore <= 0; }), found.end());
		found = found.mid(0, SampleShown);
	} else if (!m_taste.isEmpty() && found.size() > 12) {
		found = found.mid(0, std::max<qsizetype>(12, qsizetype(found.size() * 0.7)));
	} else if (queryTags.isEmpty()) {
		found = found.mid(0, PageLimit);
	}
	if (!found.isEmpty()) {
		const int group = m_nextGroup++;
		for (auto &candidate : found) {
			candidate.group = group;
			m_candidates.insert(candidate.item.key, candidate);
			m_groups[group].append(candidate.item.key);
			m_previewQueue.append(candidate.item.key);
		}
		startNextPreview();
	}
	finishIfIdle();
}

void DiscoveryFeed::startNextPreview()
{
	while (m_loaders.size() < ConcurrentPreviews && !m_previewQueue.isEmpty()) {
		const QString key = m_previewQueue.takeFirst();
		if (!m_candidates.contains(key)) {
			continue;
		}
		auto *loader = new ThumbnailLoader(m_candidates[key].item.image, QSize(320, 400), true, this);
		m_loaders.append(loader);
		// Queued: a cached or URL-less preview finishes synchronously inside load().
		connect(loader, &ThumbnailLoader::finished, this, [this, loader, key]() { previewFinished(loader, key); }, Qt::QueuedConnection);
		loader->load();
	}
}

void DiscoveryFeed::previewFinished(ThumbnailLoader *loader, const QString &key)
{
	if (!m_loaders.contains(loader)) {
		return;
	}
	m_loaders.removeAll(loader);
	loader->deleteLater();
	if (m_candidates.contains(key)) {
		auto &candidate = m_candidates[key];
		const QPixmap preview = candidate.item.image->previewImage();
		bool duplicate = preview.isNull();
		if (!duplicate) {
			candidate.fingerprint = ImageFingerprint::fromImage(preview.toImage(), candidate.item.image->size());
			for (const auto &shown : m_shownFingerprints) {
				duplicate = duplicate || candidate.fingerprint.sameImage(shown);
			}
			for (auto it = m_candidates.constBegin(); it != m_candidates.constEnd() && !duplicate; ++it) {
				duplicate = it.key() != key && it->fingerprint.valid && candidate.fingerprint.sameImage(it->fingerprint);
			}
		}
		if (duplicate) {
			candidate.fingerprint = ImageFingerprint(); // The first copy already represents it.
			candidateDone(key);
		} else if (m_encoder != nullptr && !m_visualSeeds.isEmpty()) {
			m_encoder->request(key, preview.toImage());
		} else {
			candidateDone(key);
		}
	}
	startNextPreview();
}

double DiscoveryFeed::visualScore(const QVector<float> &vector) const
{
	QList<QPair<double, double>> similarities;
	for (const auto &seed : m_visualSeeds) {
		if (seed.first.size() != vector.size()) {
			continue;
		}
		double dot = 0;
		for (int i = 0; i < vector.size(); ++i) {
			dot += double(vector[i]) * seed.first[i];
		}
		similarities.append({dot, seed.second});
	}
	std::sort(similarities.begin(), similarities.end(), [](const auto &left, const auto &right) { return left.first > right.first; });
	double sum = 0, weights = 0;
	for (const auto &similarity : similarities.mid(0, 5)) {
		sum += similarity.first * similarity.second;
		weights += similarity.second;
	}
	return weights > 0 ? sum / weights : -1;
}

void DiscoveryFeed::encoded(const QString &key, const QVector<float> &vector)
{
	if (m_candidates.contains(key)) {
		m_candidates[key].visualScore = visualScore(vector);
	}
	candidateDone(key);
}

void DiscoveryFeed::candidateDone(const QString &key)
{
	if (!m_candidates.contains(key)) {
		return;
	}
	m_candidates[key].done = true;
	const int group = m_candidates[key].group;
	for (const auto &member : m_groups.value(group)) {
		if (!m_candidates.value(member).done) {
			return;
		}
	}
	emitGroup(group, false);
	finishIfIdle();
}

void DiscoveryFeed::emitGroup(int group, bool force)
{
	QList<Candidate> ready;
	for (const auto &key : m_groups.take(group)) {
		const Candidate candidate = m_candidates.take(key);
		if ((candidate.done || force) && candidate.fingerprint.valid && !candidate.item.image->previewImage().isNull()) {
			ready.append(candidate);
		}
	}
	QVector<double> tags, visuals;
	int visualCount = 0;
	for (const auto &candidate : ready) {
		tags.append(candidate.tagScore);
		visuals.append(candidate.visualScore);
		visualCount += candidate.visualScore >= 0 ? 1 : 0;
	}
	zNormalize(tags);
	const bool useVisual = visualCount * 2 >= ready.size() && visualCount > 1;
	if (useVisual) {
		QVector<double> known;
		for (const double value : visuals) {
			if (value >= 0) {
				known.append(value);
			}
		}
		zNormalize(known);
		for (int i = 0, k = 0; i < visuals.size(); ++i) {
			visuals[i] = visuals[i] >= 0 ? known[k++] : 0;
		}
	}
	QRandomGenerator rng(m_seed ^ quint32((m_batch * 31 + group) * 2654435761u));
	for (int i = 0; i < ready.size(); ++i) {
		const double jitter = (rng.generateDouble() - 0.5) * 0.5;
		ready[i].item.score = (useVisual ? 0.5 * tags[i] + 0.5 * visuals[i] : tags[i]) + jitter;
		ready[i].item.visual = useVisual && ready[i].visualScore >= 0;
	}
	std::stable_sort(ready.begin(), ready.end(), [](const Candidate &left, const Candidate &right) { return left.item.score > right.item.score; });
	if (!m_taste.isEmpty() && ready.size() > 8) {
		ready = ready.mid(0, qsizetype(std::ceil(ready.size() * 0.8)));
	}

	QList<DiscoveryItem> items;
	for (const auto &candidate : ready) {
		bool duplicate = false;
		for (const auto &shown : m_shownFingerprints) {
			duplicate = duplicate || candidate.fingerprint.sameImage(shown); // Another source may have streamed it meanwhile.
		}
		if (duplicate) {
			continue;
		}
		items.append(candidate.item);
		m_shown.insert(candidate.item.key, candidate.item.image);
		m_shownQueries.insert(candidate.item.key, candidate.query);
		for (const QString &tag : candidate.query) {
			m_feedback[tag].shown += 1;
		}
		m_shownFingerprints.append(candidate.fingerprint);
		const QString md5 = candidate.item.image->md5().toLower();
		if (!md5.isEmpty()) {
			m_shownMd5.insert(md5);
		}
		if (!m_seenSet.contains(candidate.item.key)) {
			m_seen.append(candidate.item.key);
			m_seenSet.insert(candidate.item.key);
		}
	}
	while (m_seen.size() > MaxSeen) {
		m_seenSet.remove(m_seen.takeFirst());
	}
	if (!items.isEmpty()) {
		m_batchEmitted += int(items.size());
		emit itemsReady(items);
	}
}

void DiscoveryFeed::finishIfIdle()
{
	if (m_busy && m_pages.isEmpty() && m_groups.isEmpty()) {
		finishBatch();
	}
}

void DiscoveryFeed::finishBatch()
{
	m_timeout->stop();
	const auto pages = m_pages.keys();
	m_pages.clear();
	for (auto *page : pages) {
		disconnect(page, nullptr, this, nullptr);
		page->abort();
		page->deleteLater();
	}
	for (const auto &loader : m_loaders) {
		if (loader) {
			disconnect(loader, nullptr, this, nullptr);
			loader->abort();
			loader->deleteLater();
		}
	}
	m_loaders.clear();
	m_previewQueue.clear();
	// Timed out: show what is ready rather than waiting on the slowest source.
	for (const int group : m_groups.keys()) {
		emitGroup(group, true);
	}
	m_candidates.clear();
	if (m_backgroundChanged) {
		m_backgroundChanged = false;
		m_tasteDirty = true;
	}
	saveState();

	QString status = m_batchEmitted == 0 ? tr("No new pictures in this batch.") : QString();
	if (!m_failedSources.isEmpty()) {
		status += (status.isEmpty() ? QString() : QStringLiteral(" ")) + tr("%1 did not respond.").arg(m_failedSources.join(", "));
	}
	setBusy(false);
	emit statusChanged(status);
	if (m_batchEmitted > 0) {
		m_emptyBatches = 0;
	} else if (m_failedSources.isEmpty() && ++m_emptyBatches < 3) {
		// Everything was filtered out: try other topics, but never hammer failing sources.
		QTimer::singleShot(0, this, &DiscoveryFeed::fetchMore);
	}
}
