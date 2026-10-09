#include "models/library-recommender.h"
#include <QCryptographicHash>
#include <QMap>
#include <algorithm>
#include <cmath>

namespace
{
	QSet<QString> normalizedTags(const LibraryEntry &entry)
	{
		QSet<QString> result;
		for (const auto &tag : entry.tags()) {
			const QString normalized = tag.trimmed().toCaseFolded();
			if (!normalized.isEmpty()) {
				result.insert(normalized);
			}
		}
		return result;
	}

	double vectorNorm(const QVector<float> &values)
	{
		// Bound untrusted provider output, including vectors from a damaged cache.
		if (values.isEmpty() || values.size() > 4096) {
			return 0;
		}
		double sum = 0;
		for (const float value : values) {
			if (!std::isfinite(value)) {
				return 0;
			}
			sum += static_cast<double>(value) * value;
		}
		return sum > 0 && std::isfinite(sum) ? std::sqrt(sum) : 0;
	}

	double semanticSimilarity(const QVector<float> &left, double leftNorm, const QVector<float> &right, double rightNorm)
	{
		if (leftNorm <= 0 || rightNorm <= 0 || left.size() != right.size()) {
			return 0;
		}
		double dot = 0;
		for (qsizetype i = 0; i < left.size(); ++i) {
			dot += static_cast<double>(left[i]) * right[i];
		}
		return std::clamp(dot / (leftNorm * rightNorm), 0.0, 1.0);
	}

	struct Seed
	{
		QString key;
		QSet<QString> tags;
		QVector<float> embedding;
		double norm;
		int weight;
	};

	struct Ranked
	{
		LibraryRecommendation recommendation;
		QByteArray dailyOrder;
	};
}

LibraryRecommendationResult LibraryRecommender::rank(const QList<LibraryEntry> &candidates, const QList<LibraryEntry> &scopeSeeds,
													 const QHash<QString, QVector<float>> &embeddings, qint64 scope, const QDate &day, int limit, const QSet<QString> &hidden, quint64 rotation)
{
	LibraryRecommendationResult result;
	if (limit <= 0) {
		return result;
	}
	QHash<QString, LibraryEntry> scopedEntries;
	for (const auto &entry : scopeSeeds) {
		if (!entry.key.isEmpty()) {
			scopedEntries.insert(entry.key, entry);
		}
	}
	QList<Seed> seeds;
	QSet<QString> ratedKeys;
	int totalWeight = 0;
	int strongestWeight = 1;
	QStringList seedKeys = scopedEntries.keys();
	std::sort(seedKeys.begin(), seedKeys.end());
	for (const auto &key : seedKeys) {
		const auto &entry = scopedEntries[key];
		if (!entry.liked && !entry.favorite) {
			continue;
		}
		ratedKeys.insert(key);
		++result.ratedSeeds;
		const auto tags = normalizedTags(entry);
		const auto embedding = embeddings.value(key);
		const double norm = vectorNorm(embedding);
		if (!tags.isEmpty()) {
			++result.tagSeeds;
		}
		if (norm > 0) {
			++result.visualSeeds;
		}
		if (tags.isEmpty() && norm <= 0) {
			continue;
		}
		const int weight = entry.favorite ? 3 : 1;
		seeds.append({key, tags, embedding, norm, weight});
		totalWeight += weight;
		strongestWeight = std::max(strongestWeight, weight);
	}
	if (seeds.isEmpty()) {
		return result;
	}
	QList<Ranked> ranked;
	QSet<QString> seen;
	const QByteArray dailySalt = QByteArray::number(scope) + ':' + (day.isValid() ? day.toString(Qt::ISODate).toUtf8() : QByteArray("undated")) + ':' + QByteArray::number(rotation) + ':';
	for (const auto &entry : candidates) {
		if (entry.key.isEmpty() || seen.contains(entry.key) || ratedKeys.contains(entry.key) || hidden.contains(entry.key)) {
			continue;
		}
		seen.insert(entry.key);
		const auto tags = normalizedTags(entry);
		const auto embedding = embeddings.value(entry.key);
		const double norm = vectorNorm(embedding);
		LibraryRecommendation recommendation;
		recommendation.entry = entry;
		if (scope > 0) {
			// Candidate metadata is global; collection preference state is not.
			const auto scoped = scopedEntries.value(entry.key);
			recommendation.entry.liked = scoped.liked;
			recommendation.entry.favorite = scoped.favorite;
			recommendation.entry.notes = scoped.notes;
		}
		double strongest = 0;
		for (const auto &seed : seeds) {
			const auto shared = tags & seed.tags;
			const qsizetype unionSize = tags.size() + seed.tags.size() - shared.size();
			const double tagSimilarity = unionSize > 0 ? static_cast<double>(shared.size()) / unionSize : 0;
			const double visualSimilarity = semanticSimilarity(embedding, norm, seed.embedding, seed.norm);
			const bool hasVisual = norm > 0 && seed.norm > 0 && embedding.size() == seed.embedding.size();
			const bool hasTags = !tags.isEmpty() && !seed.tags.isEmpty();
			const double similarity = hasVisual && hasTags ? 0.8 * visualSimilarity + 0.2 * tagSimilarity
				: hasVisual ? visualSimilarity : tagSimilarity;
			const double contribution = seed.weight * similarity;
			recommendation.score += contribution;
			if (contribution > strongest) {
				strongest = contribution;
				recommendation.basedOnKey = seed.key;
				recommendation.sharedTags = shared.values();
				std::sort(recommendation.sharedTags.begin(), recommendation.sharedTags.end());
				recommendation.visual = visualSimilarity > 0;
			}
		}
		if (strongest <= 0) {
			continue;
		}
		// A clear match to one taste should survive a Library with many different tastes.
		recommendation.score = 0.7 * strongest / strongestWeight + 0.3 * recommendation.score / totalWeight;
		const auto dailyOrder = QCryptographicHash::hash(dailySalt + entry.key.toUtf8(), QCryptographicHash::Sha256);
		ranked.append({recommendation, dailyOrder});
	}
	QHash<QString, int> shownPerSeed;
	// ponytail: scan the pool per selected card; current Home views are bounded to 96 cards.
	while (!ranked.isEmpty() && result.items.size() < limit) {
		int best = 0;
		int bestBucket = -1;
		for (int i = 0; i < ranked.size(); ++i) {
			const auto &candidate = ranked[i];
			const double diversityScore = candidate.recommendation.score / (1 + 0.15 * shownPerSeed.value(candidate.recommendation.basedOnKey));
			// Only close relevance scores rotate. This ordering is not a confidence value.
			const int bucket = static_cast<int>(diversityScore * 50);
			if (bucket > bestBucket || (bucket == bestBucket && (candidate.dailyOrder < ranked[best].dailyOrder
																 || (candidate.dailyOrder == ranked[best].dailyOrder && candidate.recommendation.entry.key < ranked[best].recommendation.entry.key)))) {
				best = i;
				bestBucket = bucket;
			}
		}
		const auto selected = ranked.takeAt(best).recommendation;
		++shownPerSeed[selected.basedOnKey];
		result.items.append(selected);
	}
	return result;
}

QList<LibraryDiscoveryTopic> LibraryRecommender::topics(const QList<LibraryEntry> &scopeSeeds, const QStringList &sources, qint64 scope,
														const QDate &day, int limit, quint64 rotation)
{
	struct Topic { LibraryDiscoveryTopic value; int count = 0; double order = 0; };
	QMap<QString, Topic> pool;
	QHash<QString, int> sourceCounts;
	const QSet<QString> selectedSources(sources.begin(), sources.end());
	for (const auto &entry : scopeSeeds) {
		const QString website = entry.image.value("website").toString();
		if ((!entry.liked && !entry.favorite) || !selectedSources.contains(website)) {
			continue;
		}
		++sourceCounts[website];
		const int weight = entry.favorite ? 3 : 1;
		QSet<QString> actualTags;
		for (const auto &tag : entry.tags()) {
			actualTags.insert(tag.trimmed());
		}
		for (const auto &tag : actualTags) {
			// A source tag is data, not an instruction to add a query operator.
			if (tag.isEmpty() || tag.size() > 160 || tag.contains(':') || tag.startsWith('-') || tag.startsWith('~')
				|| std::any_of(tag.begin(), tag.end(), [](QChar ch) { return ch.isSpace() || ch.category() == QChar::Other_Control; })) {
				continue;
			}
			auto &topic = pool[website + '\n' + tag];
			topic.value.website = website;
			topic.value.tag = tag;
			topic.value.weight += weight;
			++topic.count;
		}
	}
	const QSet<QString> generic {"1girl", "1boy", "solo", "rating:safe", "rating:explicit"};
	const QByteArray salt = QByteArray::number(scope) + ':' + day.toString(Qt::ISODate).toUtf8() + ':' + QByteArray::number(rotation) + ':';
	for (auto it = pool.begin(); it != pool.end(); ++it) {
		auto &topic = it.value();
		const double prevalence = static_cast<double>(topic.count) / sourceCounts.value(topic.value.website, 1);
		topic.value.weight *= (1.1 - prevalence) * (generic.contains(topic.value.tag) ? 0.15 : 1);
		const auto digest = QCryptographicHash::hash(salt + it.key().toUtf8(), QCryptographicHash::Sha256);
		quint64 random = 0;
		for (int i = 0; i < 8; ++i) {
			random = (random << 8) | static_cast<unsigned char>(digest[i]);
		}
		const double uniform = (static_cast<double>(random >> 11) + 1) / 9007199254740993.0;
		// Weighted sampling rotates real topics without allowing common tags to drown out specific tastes.
		topic.order = -std::log(uniform) / topic.value.weight;
	}
	QList<Topic> ordered = pool.values();
	std::sort(ordered.begin(), ordered.end(), [](const Topic &left, const Topic &right) {
		return left.order != right.order ? left.order < right.order
			: left.value.website + left.value.tag < right.value.website + right.value.tag;
	});
	QList<LibraryDiscoveryTopic> result;
	for (const auto &topic : ordered.mid(0, qBound(0, limit, 3))) {
		result.append(topic.value);
	}
	return result;
}
