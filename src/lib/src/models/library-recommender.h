#ifndef LIBRARY_RECOMMENDER_H
#define LIBRARY_RECOMMENDER_H

#include <QDate>
#include <QHash>
#include <QSet>
#include <QVector>
#include "models/library-store.h"

struct LibraryRecommendation
{
	LibraryEntry entry;
	double score = 0;
	QString basedOnKey;
	QStringList sharedTags;
	bool visual = false;
};

struct LibraryRecommendationResult
{
	QList<LibraryRecommendation> items;
	int ratedSeeds = 0;
	int visualSeeds = 0;
	int tagSeeds = 0;
};

class LibraryRecommender
{
	public:
		// Embeddings must come from one model and match the current cached picture.
		static LibraryRecommendationResult rank(const QList<LibraryEntry> &candidates, const QList<LibraryEntry> &scopeSeeds,
												const QHash<QString, QVector<float>> &embeddings, qint64 scope, const QDate &day, int limit = 24, const QSet<QString> &hidden = {});
};

#endif // LIBRARY_RECOMMENDER_H
