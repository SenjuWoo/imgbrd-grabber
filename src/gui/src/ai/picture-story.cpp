#include "ai/picture-story.h"
#include <QBuffer>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSaveFile>
#include <QSettings>
#include <QTimer>
#include <QUrl>
#include "ai/story-models.h"
#include "logger.h"
#include "models/image.h"
#include "models/library-store.h"
#include "models/profile.h"
#include "models/site.h"
#include "tags/tag.h"

#include <algorithm>
#include <tuple>


namespace
{
	const QString DefaultBaseUrl = QStringLiteral("https://openrouter.ai/api/v1");
	const QString DefaultModel = QStringLiteral("openrouter/auto");
	// Long enough for a slow free model, short enough that a dead server does not hang the dialog.
	constexpr int TimeoutMs = 120000;
	// Models bill by pixels: a 4000 px original is downscaled before it is sent, both for cost and latency.
	constexpr int MaxImageEdge = 1024;
	// A JPEG that is still huge after downscaling is not worth the request; the tags alone make a story.
	constexpr int MaxImageBytes = 1500 * 1024;
	const QString KeyFile = QStringLiteral("story-key.dat");
	// A stored automatic choice is reused for a week; the provider's catalogue changes, its ids less so.
	constexpr qint64 AutoModelMaxAge = 7 * 24 * 3600;

	// Automatic choices are cached per role, each with its own provider identity: resolving a text model must
	// never make a vision model from another provider look current, or the other way round.
	struct ChoiceCache
	{
		const char *id;
		const char *base;
		const char *at;
	};
	const ChoiceCache VisionChoice {"Story/autoModelId", "Story/autoModelBase", "Story/autoModelAt"};
	const ChoiceCache TextChoice {"Story/autoTextId", "Story/autoTextBase", "Story/autoTextAt"};

	QString cachedChoice(Profile *profile, const ChoiceCache &keys)
	{
		const QSettings *settings = profile->getSettings();
		const qint64 chosen = settings->value(keys.at, 0).toLongLong();
		const QString base = settings->value(keys.base).toString();
		if (chosen <= 0 || QDateTime::currentSecsSinceEpoch() - chosen > AutoModelMaxAge || base != PictureStory::baseUrl(profile)) {
			return {};
		}
		return settings->value(keys.id).toString().trimmed();
	}

	void storeChoice(Profile *profile, const ChoiceCache &keys, const QString &model)
	{
		QSettings *settings = profile->getSettings();
		settings->setValue(keys.id, model.trimmed());
		settings->setValue(keys.base, model.isEmpty() ? QString() : PictureStory::baseUrl(profile));
		settings->setValue(keys.at, model.isEmpty() ? 0 : QDateTime::currentSecsSinceEpoch());
	}

	QString setting(Profile *profile, const QString &name)
	{
		return profile->getSettings()->value(name).toString().trimmed();
	}

	// Local imports and missing file URLs have nothing a remote model could fetch.
	QString pictureUrl(const Image &image)
	{
		const QUrl url = image.fileUrl();
		if (!url.isValid() || (url.scheme() != QLatin1String("http") && url.scheme() != QLatin1String("https"))) {
			return {};
		}
		return url.toString();
	}

	QString messageText(const QJsonObject &message)
	{
		const auto content = message.value("content");
		if (content.isString()) {
			return content.toString();
		}
		// Some providers answer with parts: [{"type":"text","text":"..."}].
		QStringList parts;
		for (const auto &part : content.toArray()) {
			const auto text = part.toObject().value("text").toString();
			if (!text.isEmpty()) {
				parts.append(text);
			}
		}
		return parts.join('\n');
	}

	// Tags that carry technical noise rather than a scene; they never help a story.
	bool isNoiseTag(const QString &tag)
	{
		static const QStringList noise {
			QStringLiteral("highres"), QStringLiteral("absurdres"), QStringLiteral("lowres"),
			QStringLiteral("commentary"), QStringLiteral("commentary_request"), QStringLiteral("translated"),
			QStringLiteral("translation_request"), QStringLiteral("tagme"), QStringLiteral("bad_id"),
			QStringLiteral("bad_pixiv_id"), QStringLiteral("bad_twitter_id"), QStringLiteral("bad_link"),
		};
		return noise.contains(tag);
	}

	// True when `keyword` appears in `tag` as a whole component ("long_hair" matches "hair", not "haircut").
	bool hasWord(const QString &tag, const QString &keyword)
	{
		return tag == keyword || tag.startsWith(keyword + '_') || tag.endsWith('_' + keyword) || tag.contains('_' + keyword + '_');
	}

	bool matchesAny(const QString &tag, const QStringList &keywords)
	{
		for (const QString &keyword : keywords) {
			if (hasWord(tag, keyword)) {
				return true;
			}
		}
		return false;
	}
}

PictureStory::PictureStory(Profile *profile, QObject *parent, QNetworkAccessManager *manager)
	: QObject(parent), m_profile(profile), m_manager(manager != nullptr ? manager : new QNetworkAccessManager(this))
{
}

PictureStory::~PictureStory()
{
	abort();
}

QString PictureStory::baseUrl(Profile *profile)
{
	const QString value = setting(profile, "Story/baseUrl");
	return value.isEmpty() ? DefaultBaseUrl : value;
}

QString PictureStory::apiKey(Profile *profile)
{
	QFile file(QDir(profile->getPath()).filePath(KeyFile));
	if (file.open(QIODevice::ReadOnly)) {
		const QString key = QString::fromUtf8(file.read(4096)).trimmed();
		if (!key.isEmpty()) {
			return key;
		}
	}

	// Older builds kept the key in the settings, and settings are part of profile backups: move it out once,
	// and only drop the old copy when the new file really holds it.
	const QString legacy = setting(profile, "Story/apiKey");
	if (!legacy.isEmpty() && setApiKey(profile, legacy)) {
		profile->getSettings()->remove("Story/apiKey");
		profile->getSettings()->sync();
	}
	return legacy;
}

bool PictureStory::setApiKey(Profile *profile, const QString &key)
{
	const QString path = QDir(profile->getPath()).filePath(KeyFile);
	if (key.trimmed().isEmpty()) {
		QFile::remove(path);
		return true;
	}
	const QByteArray bytes = key.trimmed().toUtf8();
	// Written through a save file: a partial write must not look like a stored key.
	QSaveFile file(path);
	if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		log(QStringLiteral("Story: could not save the API key to `%1`").arg(path), Logger::Warning);
		return false;
	}
	if (file.write(bytes) != bytes.size()) {
		file.cancelWriting();
		return false;
	}
	if (!file.commit()) {
		log(QStringLiteral("Story: could not finish writing the API key to `%1`").arg(path), Logger::Warning);
		return false;
	}
	// The file holds a secret: readable by its owner only where the platform allows it.
	QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
	return true;
}

QString PictureStory::model(Profile *profile)
{
	const QString value = setting(profile, "Story/model");
	return value.isEmpty() ? DefaultModel : value;
}

bool PictureStory::quality(Profile *profile)
{
	return profile->getSettings()->value("Story/quality", false).toBool();
}

void PictureStory::setQuality(Profile *profile, bool quality)
{
	profile->getSettings()->setValue("Story/quality", quality);
}

QString PictureStory::writerModel(Profile *profile)
{
	return setting(profile, "Story/writerModel");
}

void PictureStory::setWriterModel(Profile *profile, const QString &model)
{
	profile->getSettings()->setValue("Story/writerModel", model.trimmed());
}

bool PictureStory::autoModel(Profile *profile)
{
	// Local servers do not describe their models, so an automatic lookup cannot decide for them.
	const bool automatic = !isLocal(endpoint(baseUrl(profile)));
	// A model typed by the user is a decision: automatic selection only starts as the default.
	const bool typed = setting(profile, "Story/model") != DefaultModel && !setting(profile, "Story/model").isEmpty();
	return profile->getSettings()->value("Story/autoModel", automatic && !typed).toBool();
}

void PictureStory::setAutoModel(Profile *profile, bool automatic)
{
	profile->getSettings()->setValue("Story/autoModel", automatic);
}

QString PictureStory::cachedAutoModel(Profile *profile)
{
	return cachedChoice(profile, VisionChoice);
}

void PictureStory::setCachedAutoModel(Profile *profile, const QString &model)
{
	storeChoice(profile, VisionChoice, model);
}

QString PictureStory::cachedAutoTextModel(Profile *profile)
{
	return cachedChoice(profile, TextChoice);
}

void PictureStory::setCachedAutoTextModel(Profile *profile, const QString &model)
{
	storeChoice(profile, TextChoice, model);
}

QString PictureStory::preferredModel(Profile *profile)
{
	return setting(profile, "Story/preferredModel");
}

void PictureStory::setPreferredModel(Profile *profile, const QString &model)
{
	profile->getSettings()->setValue("Story/preferredModel", model.trimmed());
}

QString PictureStory::chooseModel(const QList<StoryModel> &models, bool needVision, const QString &preferred)
{
	QList<StoryModel> eligible;
	for (const StoryModel &model : models) {
		// Both modes need a model that writes text; only the picture path also needs one that reads images.
		if (!model.free || !model.text || (needVision && !model.images)) {
			continue;
		}
		// An explicit choice outranks any guessing.
		if (!preferred.isEmpty() && model.id == preferred) {
			return model.id;
		}
		eligible.append(model);
	}
	if (eligible.isEmpty()) {
		return {};
	}

	// The provider publishes no quality score, so rank by what it does publish: for tags-only a text model first
	// (cheaper and enough), then an id that names vision, then a larger context window, then a stable order.
	auto rank = [needVision](const StoryModel &model) {
		const QString id = model.id.toLower();
		const bool named = id.contains(QLatin1String("vl")) || id.contains(QLatin1String("vision")) || id.contains(QLatin1String("omni"));
		const int modality = (!needVision && !model.images) ? 0 : 1;
		return std::make_tuple(modality, named ? 0 : 1, -model.context);
	};
	std::sort(eligible.begin(), eligible.end(), [&rank](const StoryModel &a, const StoryModel &b) {
		const auto left = rank(a);
		const auto right = rank(b);
		return left == right ? a.id < b.id : left < right;
	});
	return eligible.first().id;
}

QUrl PictureStory::baseEndpoint(const QString &baseUrl)
{
	QUrl url(baseUrl.trimmed());
	if (!url.isValid() || url.host().isEmpty() || (url.scheme() != QLatin1String("http") && url.scheme() != QLatin1String("https"))) {
		return {};
	}
	// "http://localhost:1234/v1/" and "http://localhost:1234/v1" are the same server.
	QString path = url.path();
	while (path.endsWith('/')) {
		path.chop(1);
	}
	// A pasted chat endpoint names the same API root, so the model list is still found from it.
	if (path.endsWith(QLatin1String("/chat/completions"))) {
		path.chop(QStringLiteral("/chat/completions").size());
	}
	url.setPath(path);
	return url;
}

QUrl PictureStory::endpoint(const QString &baseUrl)
{
	QUrl url = baseEndpoint(baseUrl);
	if (url.isEmpty()) {
		return {};
	}
	url.setPath(url.path() + QLatin1String("/chat/completions"));
	return url;
}

QUrl PictureStory::modelsEndpoint(const QString &baseUrl)
{
	QUrl url = baseEndpoint(baseUrl);
	if (url.isEmpty()) {
		return {};
	}
	url.setPath(url.path() + QLatin1String("/models"));
	return url;
}

bool PictureStory::isLocal(const QUrl &url)
{
	const QString host = url.host().toLower();
	return host == QLatin1String("localhost") || host == QLatin1String("127.0.0.1") || host == QLatin1String("::1") || host.endsWith(QLatin1String(".localhost"));
}

QString PictureStory::settingsProblem(Profile *profile)
{
	const QUrl url = endpoint(baseUrl(profile));
	if (!url.isValid() || url.host().isEmpty()) {
		return tr("The AI base URL is not a usable http(s) address.");
	}
	if (apiKey(profile).isEmpty() && !isLocal(url)) {
		return tr("Set an API key for this provider, or point the base URL at a local server like LM Studio.");
	}
	// A key is never sent to a remote host over plain HTTP.
	if (url.scheme() == QLatin1String("http") && !isLocal(url)) {
		return tr("The AI base URL uses plain http; use https so the API key is not sent in clear text.");
	}
	return {};
}

bool PictureStory::configured(Profile *profile)
{
	return settingsProblem(profile).isEmpty();
}

void PictureStory::configure(Profile *profile, const QString &baseUrl, const QString &apiKey, const QString &model)
{
	QSettings *settings = profile->getSettings();
	settings->setValue("Story/baseUrl", baseUrl.trimmed());
	settings->setValue("Story/model", model.trimmed());
	settings->sync();
	setApiKey(profile, apiKey);
}

bool PictureStory::busy() const
{
	return m_reply != nullptr;
}

bool PictureStory::usedPicture() const
{
	return m_usedPicture;
}

bool PictureStory::visionFallback() const
{
	return m_visionFallback;
}

PictureStory::Error PictureStory::errorKind() const
{
	return m_error;
}

QString PictureStory::modelUsed() const
{
	return m_model;
}

QString PictureStory::visionModel() const
{
	return m_visionModel;
}

void PictureStory::abort()
{
	// Any reply or catalogue answer still in flight belongs to the previous run.
	++m_generation;
	if (m_catalog != nullptr) {
		m_catalog->deleteLater();
		m_catalog = nullptr;
	}
	if (m_reply != nullptr) {
		QNetworkReply *reply = m_reply;
		m_reply = nullptr;
		disconnect(reply, nullptr, this, nullptr);
		reply->abort();
		reply->deleteLater();
	}
}

QString PictureStory::imageDataUrl(const QString &path)
{
	// Only the picture the user asked about is read: nothing else from the Library leaves the machine.
	if (path.isEmpty() || !QFileInfo::exists(path) || QFileInfo(path).size() > 32 * 1024 * 1024) {
		return {};
	}
	QImageReader reader(path);
	reader.setAutoTransform(true);
	const QSize size = reader.size();
	if (size.isValid() && !size.isEmpty() && (size.width() > MaxImageEdge || size.height() > MaxImageEdge)) {
		reader.setScaledSize(size.scaled(MaxImageEdge, MaxImageEdge, Qt::KeepAspectRatio));
	}
	const QImage image = reader.read();
	if (image.isNull()) {
		return {};
	}
	QByteArray bytes;
	QBuffer buffer(&bytes);
	if (!buffer.open(QIODevice::WriteOnly) || !image.save(&buffer, "JPG", 85)) {
		return {};
	}
	if (bytes.isEmpty() || bytes.size() > MaxImageBytes) {
		return {};
	}
	return QStringLiteral("data:image/jpeg;base64,") + QString::fromLatin1(bytes.toBase64());
}

QStringList PictureStory::prioritizedTags(const QStringList &tags, int budget)
{
	// Tags that describe the scene itself. Anything matching none of these still follows, in its own order.
	static const QStringList interaction {
		"duo", "solo", "couple", "multiple_girls", "multiple_boys", "group", "hug", "kiss", "embrace",
		"holding_hands", "yuri", "yaoi", "hetero", "size_difference", "age_difference", "interspecies",
		"handjob", "fellatio", "cunnilingus", "sex", "penetration", "riding", "after_sex",
	};
	static const QStringList action {
		"standing", "sitting", "lying", "kneeling", "crouching", "leaning", "running", "walking", "jumping",
		"dancing", "sleeping", "bent_over", "spread", "arms_up", "hand_on_hip", "looking", "eye_contact",
		"stretching", "carrying", "pulling", "pushing", "biting", "sucking", "licking", "eating", "drinking",
		"reading", "playing", "fighting", "hugging", "kissing", "undressing", "bottomless", "topless",
	};
	static const QStringList clothing {
		"dress", "skirt", "shirt", "blouse", "uniform", "bikini", "swimsuit", "lingerie", "underwear",
		"panties", "bra", "stockings", "thighhighs", "gloves", "hat", "cap", "boots", "shoes", "jacket",
		"coat", "kimono", "yukata", "armor", "nude", "naked", "towel", "apron", "scarf", "necklace",
		"earrings", "choker", "suspenders", "corset", "leotard", "bodysuit", "pantyhose",
	};
	static const QStringList setting {
		"forest", "beach", "city", "street", "room", "bed", "window", "night", "sunset", "sunrise",
		"indoors", "outdoors", "water", "rain", "snow", "sky", "clouds", "garden", "park", "school",
		"library", "bath", "onsen", "ruins", "mountain", "field", "desert", "space", "pool", "kitchen",
		"office", "classroom", "cafe", "bar", "alley", "rooftop", "balcony",
	};
	static const QStringList expression {
		"smile", "grin", "blush", "tears", "crying", "angry", "sad", "surprised", "closed_eyes",
		"half-closed_eyes", "open_mouth", "tongue", "fang", "smug", "serious", "expressionless",
		"laughing", "shy", "scared", "ecstasy",
	};

	const QList<QStringList> buckets {interaction, action, clothing, setting, expression};
	QList<QStringList> picked(buckets.size());
	QStringList leftovers;

	for (const QString &tag : tags) {
		bool placed = false;
		for (int i = 0; i < buckets.size(); ++i) {
			if (matchesAny(tag, buckets[i])) {
				picked[i].append(tag);
				placed = true;
				break;
			}
		}
		if (!placed) {
			leftovers.append(tag);
		}
	}

	QStringList ordered;
	for (const QStringList &bucket : picked) {
		ordered.append(bucket);
	}
	ordered.append(leftovers);
	ordered.removeDuplicates();
	return ordered.mid(0, budget);
}

QString PictureStory::metadataBlock(const Image &image) const
{
	// Names first: who a character is and which series they come from cannot be seen in the picture, so those
	// tags are never dropped for space. The general list is reordered so actions and clothing survive the budget.
	QStringList characters, series, artists, general;
	for (const Tag &tag : image.tags()) {
		const QString type = tag.type().name();
		if (tag.text().isEmpty()) {
			continue;
		}
		if (type == QLatin1String("character")) {
			characters.append(tag.text());
		} else if (type == QLatin1String("copyright")) {
			series.append(tag.text());
		} else if (type == QLatin1String("artist")) {
			artists.append(tag.text());
		} else if (type != QLatin1String("metadata")) {
			general.append(tag.text());
		}
	}
	if (characters.isEmpty() && series.isEmpty() && general.isEmpty()) {
		// Older Library entries carry no tag types: use the plain list without the usual noise.
		for (const QString &tag : image.tagsString()) {
			if (!isNoiseTag(tag)) {
				general.append(tag);
			}
		}
	}

	QStringList lines;
	if (!characters.isEmpty()) {
		lines.append(QStringLiteral("Characters: ") + characters.join(", "));
	}
	if (!series.isEmpty()) {
		lines.append(QStringLiteral("Series: ") + series.join(", "));
	}
	if (!artists.isEmpty()) {
		lines.append(QStringLiteral("Artist: ") + artists.join(", "));
	}
	const QStringList ordered = prioritizedTags(general);
	if (!ordered.isEmpty()) {
		lines.append(QStringLiteral("Tags: ") + ordered.join(' '));
	}
	if (!image.name().isEmpty()) {
		lines.append(QStringLiteral("Title: ") + image.name());
	}
	if (image.parentSite() != nullptr) {
		lines.append(QStringLiteral("From: ") + image.parentSite()->name());
	}
	const QSize size = image.size();
	if (size.isValid() && !size.isEmpty()) {
		lines.append(QStringLiteral("Size: %1x%2").arg(size.width()).arg(size.height()));
	}
	return lines.join('\n');
}

void PictureStory::generate(const QSharedPointer<Image> &image, const QString &localPath, bool allowTagsOnly)
{
	if (image.isNull()) {
		return;
	}
	abort();
	m_image = image;
	m_key = LibraryStore::imageKey(*image);
	if (m_key.isEmpty()) {
		m_key = image->pageUrl().toString();
	}
	m_error = Error::None;
	m_usedPicture = false;
	m_visionFallback = false;
	m_scene.clear();
	m_sceneStage = false;
	m_allowTagsOnly = allowTagsOnly;
	m_quality = quality(m_profile);
	m_pictureUrl = pictureUrl(*image);
	// A downloaded copy on disk is preferred even when the picture has a URL: hotlink-blocked or
	// login-only URLs would otherwise leave the model answering from the tags alone.
	m_localDataUrl = imageDataUrl(localPath);
	m_prompt = metadataBlock(*image);
	m_model = model(m_profile);

	const QString problem = settingsProblem(m_profile);
	if (!problem.isEmpty()) {
		fail(Error::Config, problem);
		return;
	}

	if (autoModel(m_profile)) {
		// Tags-only never needs a model that can see; it needs a model that can write.
		resolveAutoModel(!m_allowTagsOnly);
		return;
	}
	startRequest();
}

void PictureStory::resolveAutoModel(bool needVision)
{
	// A recent choice is reused; otherwise the provider's own list decides, and only a free model is taken.
	const QString cached = needVision ? cachedAutoModel(m_profile) : cachedAutoTextModel(m_profile);
	if (!cached.isEmpty()) {
		m_model = cached;
		startRequest();
		return;
	}

	// The catalogue leaves the machine with a key, so it obeys the same rules as a story request.
	const QString problem = settingsProblem(m_profile);
	if (!problem.isEmpty()) {
		fail(Error::Config, problem);
		return;
	}

	const int generation = m_generation;
	// Same network manager as the story request: one place to cancel, and the catalogue is testable too.
	m_catalog = new StoryModelCatalog(m_profile, this, m_manager);
	connect(m_catalog, &StoryModelCatalog::ready, this, [this, generation, needVision](const QList<StoryModel> &models) {
		if (generation != m_generation) {
			return;
		}
		m_catalog = nullptr;
		const QString chosen = chooseModel(models, needVision, preferredModel(m_profile));
		if (chosen.isEmpty()) {
			fail(Error::InvalidModel, needVision
				? tr("No free model that reads images is listed by this provider right now. Pick one in AI story settings; a paid model is never chosen for you.")
				: tr("No free text model is listed by this provider right now. Pick one in AI story settings; a paid model is never chosen for you."));
			return;
		}
		if (needVision) {
			setCachedAutoModel(m_profile, chosen);
		} else {
			setCachedAutoTextModel(m_profile, chosen);
		}
		m_model = chosen;
		log(QStringLiteral("Story: automatic %1 model is %2").arg(needVision ? QStringLiteral("vision") : QStringLiteral("text"), chosen), Logger::Info);
		startRequest();
	});
	connect(m_catalog, &StoryModelCatalog::failed, this, [this, generation](const QString &error) {
		if (generation != m_generation) {
			return;
		}
		m_catalog = nullptr;
		fail(Error::Provider, tr("Could not look up a free model: %1").arg(error));
	});
	m_catalog->fetch();
}

void PictureStory::startRequest()
{
	// "Write from tags only" is an explicit mode: the picture is never attached, even when one is available.
	const QString picture = !m_localDataUrl.isEmpty() ? m_localDataUrl : m_pictureUrl;
	const bool pictured = !m_allowTagsOnly && !picture.isEmpty();
	if (!pictured && !m_allowTagsOnly) {
		fail(Error::NoPicture, tr("This picture has no file or address the provider could read. Download it first, or choose \"Write from tags only\"."));
		return;
	}
	// Quality mode reads the scene before writing; that needs the picture itself.
	m_sceneStage = m_quality && pictured;
	send(pictured);
}

void PictureStory::send(bool withPicture)
{
	const QUrl url = endpoint(baseUrl(m_profile));
	QNetworkRequest request(url);
	request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
	const QString key = apiKey(m_profile);
	if (!key.isEmpty()) {
		request.setRawHeader("Authorization", "Bearer " + key.toUtf8());
	}
	request.setRawHeader("X-Title", "Grabber");
	request.setTransferTimeout(TimeoutMs);

	// A local copy travels as data; otherwise the provider fetches the URL itself.
	const QString picture = !m_localDataUrl.isEmpty() ? m_localDataUrl : m_pictureUrl;
	const bool pictured = withPicture && !picture.isEmpty();
	m_pictureAttached = pictured;

	// In quality mode the writing pass may run on a different model than the one that read the picture.
	const QString writer = writerModel(m_profile);
	const QString modelToUse = (!m_sceneStage && m_quality && !writer.isEmpty()) ? writer : m_model;

	QString text = m_prompt + QStringLiteral("\n\n");
	if (m_sceneStage) {
		text += QStringLiteral("Describe only what is visible in this picture, for a writer who cannot see it. Answer with these lines, each either filled in or \"none\":\n"
							   "Characters: how many, and who they are if the names above fit\n"
							   "Actions: what each one is doing\n"
							   "Poses: how they hold themselves\n"
							   "Clothing: what each is wearing, or not wearing\n"
							   "Setting: place, time of day, light\n"
							   "Interaction: what they do to each other\n"
							   "Expression: faces and mood\n"
							   "Uncertain: anything ambiguous or conflicting\n"
							   "Only include people and details you can see. Never guess a name that is not in the list above.");
	} else if (pictured) {
		m_usedPicture = true;
		text += QStringLiteral("Write the story behind this picture. Ground it in what is visibly happening: how many characters there are, their poses, clothing, expressions and the setting. Use the names above only to say who they are. Do not add characters, change clothing, poses or actions, or invent canon lore.");
	} else if (!m_scene.isEmpty()) {
		// The writing pass receives the scene the vision pass read, never the raw tags alone.
		text += QStringLiteral("Observed scene:\n") + m_scene + QStringLiteral("\n\nWrite the story of that scene. Follow those observations exactly: do not add characters, change clothing, poses or actions, and do not contradict anything marked uncertain.");
	} else {
		m_visionFallback = true;
		text += QStringLiteral("The picture itself is not available, so write from this metadata only. Ground the scene in the tags that describe setting, poses and clothing, and do not describe details they do not support.");
	}

	QJsonArray content;
	content.append(QJsonObject {{"type", "text"}, {"text", text}});
	if (pictured) {
		content.append(QJsonObject {
			{"type", "image_url"},
			{"image_url", QJsonObject {{"url", picture}}},
		});
	}
	const QJsonObject body {
		{"model", modelToUse},
		// Free models often reason before answering; a small budget can be spent entirely on that, leaving no story.
		{"max_tokens", 2000},
		{"messages", QJsonArray {
			QJsonObject {
				{"role", "system"},
				{"content", m_sceneStage
					? QStringLiteral("You describe pictures factually and briefly, for someone who cannot see them. Only state what is visible, and mark anything unclear as uncertain. No story, no speculation, no names that were not given to you.")
					: QStringLiteral("You write the story a picture implies. Close third person or second person, present tense, 150-250 words, concrete sensory detail. No lists, no headings, no framing, no disclaimer, and never mention tags or that you are an AI. When the tags say the picture is explicit, write the adult content as explicitly as the picture suggests; otherwise follow the mood it creates.")},
			},
			QJsonObject {{"role", "user"}, {"content", content}},
		}},
	};

	m_reply = m_manager->post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
	connect(m_reply, &QNetworkReply::finished, this, &PictureStory::finished);
	// The writing pass may run on another model; the story is recorded against the one that wrote it.
	m_model = modelToUse;
	if (m_sceneStage) {
		m_visionModel = modelToUse;
	} else {
		m_writerModel = modelToUse;
	}
	log(QStringLiteral("Story: asked %1 for %2").arg(modelToUse, m_key), Logger::Info);
}

void PictureStory::finished()
{
	if (m_reply == nullptr) {
		return;
	}
	QNetworkReply *reply = m_reply;
	m_reply = nullptr;
	const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
	const QByteArray data = reply->readAll();
	const QJsonObject root = QJsonDocument::fromJson(data).object();
	const QString key = m_key;
	reply->deleteLater();

	QString text;
	const auto choices = root.value("choices").toArray();
	QString reasoning;
	if (!choices.isEmpty()) {
		const QJsonObject message = choices.first().toObject().value("message").toObject();
		text = messageText(message).trimmed();
		reasoning = message.value("reasoning").toString();
	}
	if (!text.isEmpty()) {
		if (m_sceneStage) {
			// The scene is now the evidence the prose pass writes from.
			m_scene = text;
			m_sceneStage = false;
			m_usedPicture = true;
			m_visionFallback = false;
			send(false);
			return;
		}
		emit ready(m_key, text);
		return;
	}

	QString error = root.value("error").toObject().value("message").toString();
	if (error.isEmpty()) {
		error = reply->errorString();
	}
	// A reasoning model that answered nothing but its own thinking: say so instead of "no story".
	if (text.isEmpty() && !reasoning.isEmpty() && status == 200) {
		log(QStringLiteral("Story: %1 spent the whole budget on reasoning").arg(m_model), Logger::Warning);
		fail(Error::Provider, tr("The model used its whole answer budget thinking and returned no story. Try again, or choose a model that writes without a long reasoning pass."));
		return;
	}

	// Every failure keeps its own meaning: a 404 is a wrong model, not a picture the provider refused.
	Error kind = Error::Provider;
	QString message = error.isEmpty() ? tr("The AI provider returned no story.") : error;
	if (status == 401 || status == 403) {
		kind = Error::Config;
		message = tr("The AI provider rejected the API key.");
	} else if (status == 404) {
		kind = Error::InvalidModel;
		message = tr("The model or endpoint was not found (HTTP 404): %1. Check the model id in AI story settings.").arg(error);
	} else if (status == 429) {
		kind = Error::RateLimited;
		message = tr("The provider is limiting requests right now; try again in a moment.");
	} else if (status == 400 || status == 415 || status == 422) {
		if (m_pictureAttached) {
			kind = Error::ImageRejected;
			message = tr("The provider would not accept the picture (HTTP %1). This model may not read images. Choose a vision model, or write from tags only.").arg(status);
		} else {
			kind = Error::InvalidModel;
			message = tr("The provider rejected the request (HTTP %1): %2").arg(status).arg(error);
		}
	}

	log(QStringLiteral("Story: %1 failed (%2): %3").arg(m_model).arg(status).arg(message), Logger::Warning);
	fail(kind, message);
}

void PictureStory::fail(Error kind, const QString &message)
{
	m_error = kind;
	emit failed(m_key, message);
}
