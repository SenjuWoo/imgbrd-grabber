#include "models/taste-profile.h"
#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonObject>
#include <QRandomGenerator>
#include <algorithm>
#include <functional>
#include <cmath>

namespace
{
	struct RatedTag
	{
		QString name;
		QString type;
	};

	QList<RatedTag> ratedTags(const LibraryEntry &entry)
	{
		QList<RatedTag> result;
		QSet<QString> seen;
		auto add = [&](const QString &text, const QString &type) {
			const QString name = TasteProfile::normalize(text);
			if (name.isEmpty() || seen.contains(name) || TasteProfile::isMetaTag(name, type)) {
				return;
			}
			seen.insert(name);
			result.append({name, type.isEmpty() || type == QLatin1String("unknown") ? QString() : type.toLower()});
		};
		const auto value = entry.image.value("tags");
		if (value.isString()) {
			for (const auto &tag : value.toString().split(' ', Qt::SkipEmptyParts)) {
				add(tag, {});
			}
		} else {
			for (const auto &tag : value.toArray()) {
				if (tag.isObject()) {
					const auto object = tag.toObject();
					add(object.value("text").toString(), object.value("type").toString());
				} else {
					add(tag.toString(), {});
				}
			}
		}
		for (const auto &tag : entry.image.value("local_import").toObject().value("tags").toArray()) {
			add(tag.toString(), {});
		}
		return result;
	}

	// Structural tags that describe almost every picture; they make poor searches.
	bool isGeneric(const QString &name)
	{
		static const QSet<QString> generic {
			"1girl", "1girls", "1boy", "1boys", "2girls", "2boys", "multiple_girls", "multiple_boys", "solo", "solo_focus",
			"female", "male", "human", "humanoid", "mammal", "looking_at_viewer", "smile", "blush", "open_mouth", "closed_mouth",
			"simple_background", "white_background", "transparent_background", "standing", "sitting", "full_body", "upper_body",
			"cowboy_shot", "bangs", "day", "night", "outdoors", "indoors", "holding", "text", "english_text", "dialogue",
			"speech_bubble", "hair", "eyes", "teeth", "tongue", "lips", "eyelashes", "fingernails", "navel", "collarbone",
			"long_hair", "short_hair", "medium_hair", "brown_hair", "black_hair", "blonde_hair", "blue_eyes", "brown_eyes",
			"red_eyes", "green_eyes", "nude", "naked", "clothed", "clothing", "female_focus", "male_focus", "duo", "group",
			"looking_back", "from_behind", "from_side", "from_above", "from_below", "rating:safe", "rating:questionable", "rating:explicit",
		};
		return generic.contains(name);
	}

	double typeBoost(const QString &type)
	{
		if (type == QLatin1String("artist")) {
			return 1.6;
		}
		if (type == QLatin1String("character")) {
			return 1.4;
		}
		if (type == QLatin1String("copyright")) {
			return 1.15;
		}
		return type.isEmpty() ? 0.9 : 1.0;
	}

	// A source tag is data, not an instruction to add a search operator.
	bool queryable(const QString &name)
	{
		return !name.isEmpty() && name.size() <= 160 && !name.contains(':') && !name.startsWith('-') && !name.startsWith('~')
			&& std::none_of(name.begin(), name.end(), [](QChar ch) { return ch.isSpace() || ch.category() == QChar::Other_Control; });
	}

	bool crossSiteType(const QString &type)
	{
		return type == QLatin1String("artist") || type == QLatin1String("character") || type == QLatin1String("copyright");
	}

	double recency(const QString &savedAt, const QDateTime &now)
	{
		const QDateTime saved = QDateTime::fromString(savedAt, Qt::ISODateWithMs);
		if (!saved.isValid() || !now.isValid()) {
			return 1.0;
		}
		const double days = std::max<qint64>(0, saved.daysTo(now));
		return 0.4 + 0.6 * std::exp(-days / 120.0);
	}

	template <typename T>
	QStringList sortedKeys(const QHash<QString, T> &hash)
	{
		QStringList keys = hash.keys();
		std::sort(keys.begin(), keys.end());
		return keys;
	}

	// Sources sampled this much are trusted for tag frequencies; samples beyond the cap fade so tastes in posts can drift.
	constexpr double MinSourcePosts = 60;
	constexpr double MaxSourcePosts = 4000;
	constexpr int MaxSourceTags = 3000;

	// Expected share of shown pictures that get rated, and how many showings that guess is worth.
	constexpr double FeedbackBaseline = 0.04;
	constexpr double FeedbackPrior = 25;

	// Searches come from the strongest tags only; hundreds of faint ones otherwise outweigh the few real themes.
	constexpr int QueryPool = 30;

	double feedbackBoost(const QHash<QString, DiscoveryFeedback> &feedback, const QString &name)
	{
		const auto it = feedback.constFind(name);
		if (it == feedback.constEnd()) {
			return 1.0;
		}
		const double rate = (it->rated + FeedbackBaseline * FeedbackPrior) / (std::max(0.0, it->shown) + FeedbackPrior);
		return std::clamp(rate / FeedbackBaseline, 0.2, 4.0);
	}

	QString weightedPick(const QHash<QString, double> &weights, QRandomGenerator &rng)
	{
		double total = 0;
		for (const double weight : weights) {
			total += std::max(0.0, weight);
		}
		if (total <= 0) {
			return {};
		}
		double target = rng.generateDouble() * total;
		const QStringList keys = sortedKeys(weights);
		for (const QString &key : keys) {
			target -= std::max(0.0, weights[key]);
			if (target <= 0) {
				return key;
			}
		}
		return keys.last();
	}
}

void TagBackground::add(const QString &website, const QStringList &tags)
{
	if (website.isEmpty()) {
		return;
	}
	auto &counts = m_counts[website];
	QSet<QString> unique;
	for (const QString &tag : tags) {
		const QString name = TasteProfile::normalize(tag);
		if (!name.isEmpty() && !unique.contains(name)) {
			unique.insert(name);
			counts[name] += 1;
		}
	}
	double &posts = m_posts[website];
	posts += 1;
	if (posts > MaxSourcePosts) {
		posts /= 2;
		for (auto it = counts.begin(); it != counts.end();) {
			it.value() /= 2;
			it = it.value() < 0.5 ? counts.erase(it) : std::next(it);
		}
	}
	if (counts.size() > MaxSourceTags * 3 / 2) {
		QList<double> values = counts.values();
		std::nth_element(values.begin(), values.begin() + MaxSourceTags, values.end(), std::greater<double>());
		const double threshold = values[MaxSourceTags];
		for (auto it = counts.begin(); it != counts.end();) {
			it = it.value() <= threshold ? counts.erase(it) : std::next(it);
		}
	}
}

double TagBackground::frequency(const QString &tag, const QHash<QString, double> &ratedOn) const
{
	if (ratedOn.isEmpty()) {
		double result = -1;
		for (auto it = m_posts.constBegin(); it != m_posts.constEnd(); ++it) {
			if (it.value() >= MinSourcePosts) {
				result = std::max(result, m_counts.value(it.key()).value(tag) / it.value());
			}
		}
		return result;
	}
	double sum = 0, covered = 0, total = 0;
	for (auto it = ratedOn.constBegin(); it != ratedOn.constEnd(); ++it) {
		total += it.value();
		const double posts = m_posts.value(it.key());
		if (posts >= MinSourcePosts) {
			sum += it.value() * m_counts.value(it.key()).value(tag) / posts;
			covered += it.value();
		}
	}
	return covered > 0 && covered * 2 >= total ? sum / covered : -1;
}

QHash<QString, double> TagBackground::common(double minimumShare) const
{
	QHash<QString, double> result;
	for (auto site = m_posts.constBegin(); site != m_posts.constEnd(); ++site) {
		if (site.value() < MinSourcePosts) {
			continue;
		}
		const auto &counts = m_counts[site.key()];
		for (auto it = counts.constBegin(); it != counts.constEnd(); ++it) {
			const double share = it.value() / site.value();
			if (share >= minimumShare && share > result.value(it.key())) {
				result.insert(it.key(), share);
			}
		}
	}
	return result;
}

double TagBackground::posts(const QString &website) const
{
	if (!website.isEmpty()) {
		return m_posts.value(website);
	}
	double best = 0;
	for (const double posts : m_posts) {
		best = std::max(best, posts);
	}
	return best;
}

QJsonObject TagBackground::toJson() const
{
	QJsonObject sites;
	for (auto site = m_posts.constBegin(); site != m_posts.constEnd(); ++site) {
		QJsonObject tags;
		const auto &counts = m_counts[site.key()];
		for (auto it = counts.constBegin(); it != counts.constEnd(); ++it) {
			tags.insert(it.key(), it.value());
		}
		sites.insert(site.key(), QJsonObject {{"posts", site.value()}, {"tags", tags}});
	}
	return sites;
}

TagBackground TagBackground::fromJson(const QJsonObject &json)
{
	TagBackground result;
	for (auto site = json.constBegin(); site != json.constEnd(); ++site) {
		const auto object = site.value().toObject();
		const double posts = object.value("posts").toDouble();
		if (site.key().isEmpty() || posts <= 0 || posts > MaxSourcePosts) {
			continue;
		}
		result.m_posts.insert(site.key(), posts);
		auto &counts = result.m_counts[site.key()];
		const auto tags = object.value("tags").toObject();
		for (auto it = tags.constBegin(); it != tags.constEnd() && counts.size() < MaxSourceTags * 3 / 2; ++it) {
			const double count = it.value().toDouble();
			if (count > 0) {
				counts.insert(it.key(), std::min(count, posts));
			}
		}
	}
	return result;
}

QString TasteProfile::normalize(const QString &tag)
{
	QString result = tag.trimmed().toCaseFolded();
	result.replace(' ', '_');
	return result;
}

bool TasteProfile::isMetaTag(const QString &name, const QString &type)
{
	static const QSet<QString> meta {
		"highres", "absurdres", "lowres", "hi_res", "absurd_res", "high_res", "4k", "8k", "hd", "2d", "commentary", "commentary_request",
		"english_commentary", "translated", "translation_request", "partially_translated", "check_translation", "tagme", "md5_mismatch",
		"duplicate", "revision", "alternate_version_available", "alternate_version_at_source", "variant_set", "image_sample", "jpeg_artifacts",
		"watermark", "signature", "artist_name", "dated", "web_address", "logo", "character_name", "copyright_name", "official_art",
		"digital_media_(artwork)", "traditional_media", "third-party_edit", "sound", "webm", "mp4", "gif", "animated", "video",
		"ai_generated", "ai-generated", "ai_assisted", "stable_diffusion", "novelai", "nai_diffusion", "tall_image", "wide_image",
	};
	if (name.isEmpty() || type.compare(QLatin1String("meta"), Qt::CaseInsensitive) == 0 || meta.contains(name)) {
		return true;
	}
	return name.endsWith(QLatin1String("_request")) || name.startsWith(QLatin1String("commentary")) || name.startsWith(QLatin1String("bad_"))
		|| name.contains(QLatin1String("resolution")) || name.endsWith(QLatin1String("_username")) || name.endsWith(QLatin1String("_id"))
		|| name.startsWith(QLatin1String("rating:"));
}

TasteProfile TasteProfile::build(const QList<LibraryEntry> &entries, const QHash<QString, double> &dislikedTags, const QDateTime &now, const TagBackground &background)
{
	TasteProfile profile;
	struct Seed
	{
		QString key;
		QString website;
		double weight;
		QList<RatedTag> tags;
	};
	QList<Seed> seeds;
	QHash<QString, int> documentFrequency;
	QHash<QString, double> termWeight;
	QHash<QString, QString> types;
	double totalWeight = 0;
	for (const auto &entry : entries) {
		if (!entry.liked && !entry.favorite) {
			continue;
		}
		entry.favorite ? ++profile.m_favorites : ++profile.m_likes;
		const double weight = (entry.favorite ? 3.0 : 1.0) * recency(entry.savedAt, now);
		profile.m_seedWeights.insert(entry.key, weight);
		Seed seed {entry.key, entry.image.value("website").toString(), weight, ratedTags(entry)};
		if (!seed.website.isEmpty()) {
			profile.m_siteWeights[seed.website] += weight;
		}
		if (seed.tags.isEmpty()) {
			continue;
		}
		totalWeight += weight;
		for (const auto &tag : seed.tags) {
			++documentFrequency[tag.name];
			termWeight[tag.name] += weight;
			if (!tag.type.isEmpty() && types.value(tag.name).isEmpty()) {
				types.insert(tag.name, tag.type);
			}
			if (!seed.website.isEmpty()) {
				profile.m_sites[tag.name][seed.website] += weight;
			}
		}
		seeds.append(seed);
	}
	if (seeds.isEmpty()) {
		return profile;
	}

	const double documents = seeds.size();
	const double sampled = background.posts();
	const bool contrast = sampled >= MinSourcePosts;
	auto inverse = [&](const QString &name) { return std::log((documents + 1.0) / (documentFrequency.value(name) + 0.5)); };
	double strongest = 0;
	for (const QString &name : sortedKeys(termWeight)) {
		const QString type = types.value(name);
		double weight = 0;
		if (contrast) {
			// General tags depend on each site's vocabulary ("1girls" vs "1girl"): compare them only where they were rated.
			const double frequency = background.frequency(name, crossSiteType(type) ? QHash<QString, double>() : profile.m_sites.value(name));
			if (frequency < 0) {
				profile.m_tags.insert(name, {name, type, 0, documentFrequency.value(name)}); // Unknown until those sites are sampled.
				continue;
			}
			// Lift: a theme in 20% of ratings but 1% of posts says far more than breasts in 60% of both.
			const double base = std::max(frequency, 0.5 / sampled);
			const double lift = std::clamp(std::log(termWeight.value(name) / totalWeight / base), -2.0, 6.0);
			const double confidence = 1.0 - std::exp(-documentFrequency.value(name) / 3.0);
			weight = confidence * lift * (lift > 0 ? typeBoost(type) : 1.0);
		} else {
			// Ubiquitous liked tags still matter a little; one-off general tags are mostly noise.
			const bool singleton = documentFrequency.value(name) < 2 && !crossSiteType(type) && termWeight.value(name) < 3.0;
			weight = termWeight.value(name) / totalWeight * (0.3 + inverse(name)) * typeBoost(type);
			if (singleton) {
				weight *= 0.35;
			}
			if (isGeneric(name)) {
				weight *= 0.5;
			}
		}
		profile.m_tags.insert(name, {name, type, weight, documentFrequency.value(name)});
		strongest = std::max(strongest, weight);
	}
	if (contrast) {
		// Common tags that never show up in ratings are mild signs of disinterest.
		const auto common = background.common(0.02);
		for (auto it = common.constBegin(); it != common.constEnd(); ++it) {
			if (!termWeight.contains(it.key()) && !isMetaTag(it.key())) {
				const double expected = it.value() * documents;
				const double lift = std::clamp(std::log(0.5 / documents / it.value()), -2.0, 0.0);
				profile.m_tags.insert(it.key(), {it.key(), {}, (1.0 - std::exp(-expected / 3.0)) * lift, 0});
			}
		}
	}
	strongest = strongest > 0 ? strongest : 1.0;
	for (auto it = profile.m_tags.begin(); it != profile.m_tags.end(); ++it) {
		it->weight = std::max(-1.0, it->weight / strongest);
	}

	// Pair tags that appear together so searches stay coherent with real tastes.
	for (const auto &seed : seeds) {
		QList<QPair<double, QString>> ranked;
		for (const auto &tag : seed.tags) {
			if (!isGeneric(tag.name)) {
				ranked.append({profile.m_tags.value(tag.name).weight, tag.name});
			}
		}
		std::sort(ranked.begin(), ranked.end(), [](const auto &left, const auto &right) { return left.first != right.first ? left.first > right.first : left.second < right.second; });
		ranked = ranked.mid(0, 24);
		for (const auto &left : ranked) {
			for (const auto &right : ranked) {
				if (left.second != right.second) {
					profile.m_cooccurrences[left.second][right.second] += seed.weight;
				}
			}
		}
	}

	for (auto it = dislikedTags.constBegin(); it != dislikedTags.constEnd(); ++it) {
		const QString name = normalize(it.key());
		if (name.isEmpty() || it.value() <= 0) {
			continue;
		}
		auto &tag = profile.m_tags[name];
		tag.name = name;
		// A hidden picture weakly penalizes its tags, except tags that clearly belong to this taste.
		const double positive = std::max(0.0, tag.weight);
		tag.weight -= 0.3 * std::log1p(it.value()) * (1.0 - std::min(1.0, positive / 0.3));
		tag.weight = std::max(-1.0, tag.weight);
	}

	double squares = 0;
	for (const auto &tag : profile.m_tags) {
		squares += tag.weight > 0 ? tag.weight * tag.weight : 0;
	}
	profile.m_norm = std::sqrt(squares);
	return profile;
}

bool TasteProfile::isEmpty() const { return m_norm <= 0; }
int TasteProfile::likes() const { return m_likes; }
int TasteProfile::favorites() const { return m_favorites; }
const QHash<QString, double> &TasteProfile::seedWeights() const { return m_seedWeights; }
const QHash<QString, double> &TasteProfile::siteWeights() const { return m_siteWeights; }

double TasteProfile::weight(const QString &tag) const
{
	return m_tags.value(normalize(tag)).weight;
}

QList<TasteTag> TasteProfile::topTags(int limit, const QSet<QString> &types) const
{
	QList<TasteTag> result;
	for (const auto &tag : m_tags) {
		if (tag.weight > 0 && (types.isEmpty() || types.contains(tag.type))) {
			result.append(tag);
		}
	}
	std::sort(result.begin(), result.end(), [](const TasteTag &left, const TasteTag &right) {
		return left.weight != right.weight ? left.weight > right.weight : left.name < right.name;
	});
	return result.mid(0, std::max(0, limit));
}

double TasteProfile::score(const QStringList &tags) const
{
	if (m_norm <= 0) {
		return 0;
	}
	QSet<QString> unique;
	double sum = 0;
	for (const QString &tag : tags) {
		const QString name = normalize(tag);
		if (isMetaTag(name) || unique.contains(name)) {
			continue;
		}
		unique.insert(name);
		sum += m_tags.value(name).weight;
	}
	return unique.isEmpty() ? 0 : sum / (std::sqrt(double(unique.size())) * m_norm);
}

QList<DiscoveryQuery> TasteProfile::queries(const QStringList &sources, int count, quint32 seed, const QHash<QString, DiscoveryFeedback> &feedback) const
{
	QList<DiscoveryQuery> result;
	QStringList available = sources;
	available.removeDuplicates();
	available.removeAll(QString());
	std::sort(available.begin(), available.end());
	if (available.isEmpty() || count <= 0) {
		return result;
	}
	QRandomGenerator rng(seed);
	if (isEmpty()) {
		// Cold start: newest posts, spread over the selected sources.
		std::shuffle(available.begin(), available.end(), rng);
		for (int i = 0; i < count; ++i) {
			result.append({available[i % available.size()], {}, QCoreApplication::translate("TasteProfile", "Fresh from %1").arg(available[i % available.size()])});
		}
		return result;
	}

	const QSet<QString> selected(available.begin(), available.end());
	QList<QPair<double, QString>> eligible;
	for (const QString &name : sortedKeys(m_tags)) {
		const auto &tag = m_tags[name];
		if (tag.weight <= 0.02 || isGeneric(name) || !queryable(name)) {
			continue;
		}
		bool onSelected = false;
		for (auto it = m_sites.value(name).constBegin(); it != m_sites.value(name).constEnd(); ++it) {
			onSelected = onSelected || selected.contains(it.key());
		}
		if (!onSelected && !crossSiteType(tag.type)) {
			continue;
		}
		eligible.append({tag.weight * tag.weight * typeBoost(tag.type) * feedbackBoost(feedback, name), name});
	}
	const auto byPriority = [](const auto &left, const auto &right) { return left.first != right.first ? left.first > right.first : left.second < right.second; };
	std::sort(eligible.begin(), eligible.end(), byPriority);
	QList<QPair<double, QString>> pool;
	for (const auto &entry : eligible.mid(0, QueryPool)) {
		// Weighted sampling without replacement (Efraimidis-Spirakis).
		pool.append({std::pow(std::max(1e-12, rng.generateDouble()), 1.0 / entry.first), entry.second});
	}
	std::sort(pool.begin(), pool.end(), byPriority);

	for (const auto &entry : pool.mid(0, count)) {
		const QString &primary = entry.second;
		const auto &tag = m_tags[primary];
		QHash<QString, double> sites;
		for (auto it = m_sites.value(primary).constBegin(); it != m_sites.value(primary).constEnd(); ++it) {
			if (selected.contains(it.key())) {
				sites.insert(it.key(), it.value());
			}
		}
		QString website = weightedPick(sites, rng);
		if (website.isEmpty()) {
			website = available[int(rng.bounded(quint32(available.size())))];
		}
		DiscoveryQuery query {website, {primary}, QCoreApplication::translate("TasteProfile", "Because you like %1").arg(primary)};
		const double pairChance = crossSiteType(tag.type) ? 0.35 : 0.75;
		if (rng.generateDouble() < pairChance) {
			QHash<QString, double> partners;
			for (auto it = m_cooccurrences.value(primary).constBegin(); it != m_cooccurrences.value(primary).constEnd(); ++it) {
				const auto partner = m_tags.value(it.key());
				// Partners must exist on the chosen source so both tags share one vocabulary.
				if (partner.weight > 0.05 && !isGeneric(partner.name) && queryable(partner.name) && m_sites.value(partner.name).contains(website)
					&& !(tag.type == QLatin1String("artist") && partner.type == QLatin1String("artist"))) {
					partners.insert(partner.name, it.value() * partner.weight * feedbackBoost(feedback, partner.name));
				}
			}
			const QString partner = weightedPick(partners, rng);
			if (!partner.isEmpty()) {
				query.tags.append(partner);
				query.reason = QCoreApplication::translate("TasteProfile", "Because you like %1 and %2").arg(primary, partner);
			}
		}
		result.append(query);
	}
	return result;
}
