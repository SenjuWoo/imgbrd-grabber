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
	/** For another spelling of a learned tag on some source, the learned tag it stands for. */
	QString sameAs;
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

/** What the user told Discover: tags to see more or less of, today's focus and how far to stray from known favorites. */
struct TasteTuning
{
	/** Neutral: neither searched for nor held against a picture, for tags learned by mistake. */
	enum Preference { Never = -2, Less = -1, More = 1, Neutral = 2 };
	enum Variety { Familiar = 0, Balanced = 1, Adventurous = 2 };

	QHash<QString, int> tags;
	QStringList focus;
	int variety = Balanced;

	bool isNeutral() const;
	QJsonObject toJson() const;
	static TasteTuning fromJson(const QJsonObject &json);
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

		/** Share of a source's sampled posts carrying the tag, 0 when unknown. */
		double share(const QString &website, const QString &tag) const;

		/** Every tag seen on a sampled source. */
		QSet<QString> vocabulary() const;

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
		static TasteProfile build(const QList<LibraryEntry> &entries, const QHash<QString, double> &dislikedTags = {}, const QDateTime &now = QDateTime::currentDateTimeUtc(), const TagBackground &background = {}, const TasteTuning &tuning = {});
		static QString normalize(const QString &tag);
		static bool isMetaTag(const QString &name, const QString &type = {});
		/** Groups of tags that name the same thing on different sources, such as hidden_sex_toy and stealth_sex_toy. */
		static const QList<QStringList> &synonymGroups();

		bool isEmpty() const;
		int likes() const;
		int favorites() const;
		double weight(const QString &tag) const;
		/** What was learned about a tag: strength and how many of the user's rated pictures carry it. Empty name when unknown. */
		TasteTag info(const QString &tag) const;
		QList<TasteTag> topTags(int limit, const QSet<QString> &types = {}) const;
		/** Tags Discover learned to avoid, most avoided first. */
		QList<TasteTag> avoidedTags(int limit) const;
		/** The candidate's tags that match this taste best, strongest first. */
		QStringList matches(const QStringList &tags, int limit) const;
		/** Whether the tags include one the user never wants to see. */
		bool blocks(const QStringList &tags) const;
		/** Other names of this tag seen in ratings or on sampled sources (synonyms and other spellings). */
		QStringList equivalents(const QString &tag) const;
		/**
		 * One search term matching any of the names, in the source's own OR syntax; empty when the source has none.
		 * @param engine Source engine name ("Gelbooru (0.2)", "E621", ...).
		 * @param terms  Search terms counting the group as one.
		 */
		static QString anyOf(const QString &engine, const QString &host, const QStringList &names, int terms);
		const TasteTuning &tuning() const;
		/** Searches for new pictures; tags whose earlier results were shown but never rated are picked less often. */
		QList<DiscoveryQuery> queries(const QStringList &sources, int count, quint32 seed, const QHash<QString, DiscoveryFeedback> &feedback = {}) const;

		/** Rating weight per source website. */
		const QHash<QString, double> &siteWeights() const;

		/** Cosine-like match of a candidate's tags against this profile; negative when it matches disliked tags. */
		double score(const QStringList &tags) const;

		/** Rated picture key to its rating weight, used to weigh visual seeds. */
		const QHash<QString, double> &seedWeights() const;

	private:
		void applyTuning();
		void learnSynonyms(const TagBackground &background, const QHash<QString, int> &rated);
		QStringList group(const QString &name) const;
		bool representative(const QString &name) const;

		QHash<QString, TasteTag> m_tags;
		QHash<QString, QHash<QString, double>> m_cooccurrences;
		QHash<QString, QHash<QString, double>> m_sites;
		QHash<QString, double> m_seedWeights;
		QHash<QString, double> m_siteWeights;
		TasteTuning m_tuning;
		QSet<QString> m_pinned;
		QSet<QString> m_blocked;
		QHash<QString, int> m_groupOf;
		QList<QStringList> m_groups;
		QHash<QString, QHash<QString, double>> m_variantShares;
		QSet<QString> m_known;
		double m_norm = 0;
		int m_likes = 0;
		int m_favorites = 0;
};

#endif // TASTE_PROFILE_H
