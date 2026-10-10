#ifndef TASTE_PROFILE_H
#define TASTE_PROFILE_H

#include <QDateTime>
#include <QHash>
#include <QJsonObject>
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

/** How often Discover showed pictures found through a tag, and how many of them were then liked or favorited. */
struct DiscoveryFeedback
{
	double shown = 0;
	double rated = 0;
};

/**
 * How common each tag is on each source, sampled from its newest posts.
 * It tells a distinctive taste (a theme, an artist) apart from tags almost every post has.
 */
class TagBackground
{
	public:
		void add(const QString &website, const QStringList &tags);

		/**
		 * Share of sampled posts carrying the tag: the largest over well-sampled sources, or with `ratedOn` (website to rating weight)
		 * the average over those sources weighted by ratings. -1 while sampled sources cover too few of them.
		 */
		double frequency(const QString &tag, const QHash<QString, double> &ratedOn = {}) const;

		/** Tags carried by at least this share of posts on a well-sampled source, with that share. */
		QHash<QString, double> common(double minimumShare) const;

		/** Posts sampled from a source, or the best-sampled source's count without one. */
		double posts(const QString &website = {}) const;

		QJsonObject toJson() const;
		static TagBackground fromJson(const QJsonObject &json);

	private:
		QHash<QString, QHash<QString, double>> m_counts;
		QHash<QString, double> m_posts;
};

/**
 * Tag preferences learned from liked and favorited pictures.
 * Favorites count three times as much as likes, recent ratings slightly more than old ones.
 * With a tag background, a tag weighs by how much more often it appears in rated pictures than on its sources.
 */
class TasteProfile
{
	public:
		static TasteProfile build(const QList<LibraryEntry> &entries, const QHash<QString, double> &dislikedTags = {}, const QDateTime &now = QDateTime::currentDateTimeUtc(), const TagBackground &background = {});
		static QString normalize(const QString &tag);
		static bool isMetaTag(const QString &name, const QString &type = {});

		bool isEmpty() const;
		int likes() const;
		int favorites() const;
		double weight(const QString &tag) const;
		QList<TasteTag> topTags(int limit, const QSet<QString> &types = {}) const;
		/** Searches for new pictures; tags whose earlier results were shown but never rated are picked less often. */
		QList<DiscoveryQuery> queries(const QStringList &sources, int count, quint32 seed, const QHash<QString, DiscoveryFeedback> &feedback = {}) const;

		/** Rating weight per source website. */
		const QHash<QString, double> &siteWeights() const;

		/** Cosine-like match of a candidate's tags against this profile; negative when it matches disliked tags. */
		double score(const QStringList &tags) const;

		/** Rated picture key to its rating weight, used to weigh visual seeds. */
		const QHash<QString, double> &seedWeights() const;

	private:
		QHash<QString, TasteTag> m_tags;
		QHash<QString, QHash<QString, double>> m_cooccurrences;
		QHash<QString, QHash<QString, double>> m_sites;
		QHash<QString, double> m_seedWeights;
		QHash<QString, double> m_siteWeights;
		double m_norm = 0;
		int m_likes = 0;
		int m_favorites = 0;
};

#endif // TASTE_PROFILE_H
