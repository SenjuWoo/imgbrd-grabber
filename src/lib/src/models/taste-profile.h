#ifndef TASTE_PROFILE_H
#define TASTE_PROFILE_H

#include <QDateTime>
#include <QHash>
#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>
#include "models/library-store.h"

struct TasteTag
{
	QString name;
	QString type;
	double weight = 0;
	int count = 0;
};

struct DiscoveryQuery
{
	QString website;
	QStringList tags;
	QString reason;
};

/**
 * Tag preferences learned from liked and favorited pictures.
 * Favorites count three times as much as likes, recent ratings slightly more than old ones.
 */
class TasteProfile
{
	public:
		static TasteProfile build(const QList<LibraryEntry> &entries, const QHash<QString, double> &dislikedTags = {}, const QDateTime &now = QDateTime::currentDateTimeUtc());
		static QString normalize(const QString &tag);
		static bool isMetaTag(const QString &name, const QString &type = {});

		bool isEmpty() const;
		int likes() const;
		int favorites() const;
		double weight(const QString &tag) const;
		QList<TasteTag> topTags(int limit, const QSet<QString> &types = {}) const;
		QList<DiscoveryQuery> queries(const QStringList &sources, int count, quint32 seed) const;

		/** Cosine-like match of a candidate's tags against this profile; negative when it matches disliked tags. */
		double score(const QStringList &tags) const;

		/** Rated picture key to its rating weight, used to weigh visual seeds. */
		const QHash<QString, double> &seedWeights() const;

	private:
		QHash<QString, TasteTag> m_tags;
		QHash<QString, QHash<QString, double>> m_cooccurrences;
		QHash<QString, QHash<QString, double>> m_sites;
		QHash<QString, double> m_seedWeights;
		double m_norm = 0;
		int m_likes = 0;
		int m_favorites = 0;
};

#endif // TASTE_PROFILE_H
