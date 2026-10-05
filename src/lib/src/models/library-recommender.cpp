#include "models/library-recommender.h"
#include <QCryptographicHash>
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
		int bucket;
		QByteArray dailyOrder;
	};
}

LibraryRecommendationResult LibraryRecommender::rank(const QList<LibraryEntry> &candidates, const QList<LibraryEntry> &scopeSeeds,
													 const QHash<QString, QVector<float>> &embeddings, qint64 scope, const QDate &day, int limit, const QSet<QString> &hidden)
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
	}
	if (seeds.isEmpty()) {
		return result;
	}
	QList<Ranked> ranked;
	QSet<QString> seen;
	const QByteArray dailySalt = QByteArray::number(scope) + ':' + (day.isValid() ? day.toString(Qt::ISODate).toUtf8() : QByteArray("undated")) + ':';
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
		recommendation.score /= totalWeight;
		// Close scores rotate deterministically each day; they are not confidence values.
		const int bucket = static_cast<int>(recommendation.score * 50);
		const auto dailyOrder = QCryptographicHash::hash(dailySalt + entry.key.toUtf8(), QCryptographicHash::Sha256);
		ranked.append({recommendation, bucket, dailyOrder});
	}
	std::sort(ranked.begin(), ranked.end(), [](const Ranked &left, const Ranked &right) {
		if (left.bucket != right.bucket) {
			return left.bucket > right.bucket;
		}
		if (left.dailyOrder != right.dailyOrder) {
			return left.dailyOrder < right.dailyOrder;
		}
		return left.recommendation.entry.key < right.recommendation.entry.key;
	});
	for (int i = 0; i < std::min(limit, static_cast<int>(ranked.size())); ++i) {
		result.items.append(ranked[i].recommendation);
	}
	return result;
}
