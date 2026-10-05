#include "library-recommendations.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSaveFile>
#include <QSettings>
#include <QTimer>
#include <QtConcurrent>
#include <cmath>
#include "models/library-image-encoder.h"
#include "models/profile.h"
#include "utils/file-utils.h"

namespace {
	QString fingerprint(const QByteArray &thumbnail)
	{
		return QString::fromLatin1(QCryptographicHash::hash(thumbnail, QCryptographicHash::Sha256).toHex());
	}

	bool validVector(const QVector<float> &vector)
	{
		if (vector.size() != 512) {
			return false;
		}
		double norm = 0;
		for (float value : vector) {
			if (!std::isfinite(value)) {
				return false;
			}
			norm += double(value) * value;
		}
		return std::abs(norm - 1.0) < 0.01;
	}

	QString hiddenSetting(qint64 scope)
	{
		return QString("recommendations/hidden/%1").arg(scope);
	}

	QString saveCache(const QString &path, const QHash<QString, QVector<float>> &vectors, const QHash<QString, QString> &fingerprints)
	{
		QJsonObject records;
		for (auto it = vectors.constBegin(); it != vectors.constEnd(); ++it) {
			if (!validVector(it.value())) {
				continue;
			}
			QJsonArray vector;
			for (float value : it.value()) {
				vector.append(double(value));
			}
			records.insert(it.key(), QJsonObject {{"thumbnail", fingerprints.value(it.key())}, {"vector", vector}});
		}
		const QJsonObject root {{"version", 1}, {"model", LibraryImageEncoder::modelId()}, {"model_sha256", LibraryImageEncoder::modelSha256()}, {"entries", records}};
		const auto bytes = QJsonDocument(root).toJson(QJsonDocument::Compact);
		if (bytes.size() > 64 * 1024 * 1024) {
			return LibraryRecommendations::tr("Local AI index exceeds the 64 MiB cache limit. Existing saved index was preserved.");
		}
		if (!ensureFileParent(path) || !safeWriteFile(path, bytes)) {
			return LibraryRecommendations::tr("Could not save the local AI index. Existing saved index was preserved.");
		}
		return {};
	}
}

LibraryRecommendations::LibraryRecommendations(Profile *profile, QObject *parent)
	: QObject(parent), m_profile(profile), m_watcher(new QFutureWatcher<LibraryIndexResult>(this)), m_reindex(new QTimer(this))
{
	m_reindex->setSingleShot(true);
	m_reindex->setInterval(1000);
	connect(m_reindex, &QTimer::timeout, this, &LibraryRecommendations::startIndexing);
	loadCache();
	m_status = modelAvailable() ? tr("Local AI index: %1 pictures. Update to include new pictures.").arg(indexedCount())
							   : tr("Set up local AI to recommend pictures without tags. Model download: 89 MB.");
	connect(profile->library(), &LibraryStore::imageChanged, this, &LibraryRecommendations::invalidate);
	connect(m_watcher, &QFutureWatcher<LibraryIndexResult>::finished, this, [this]() {
		if (m_indexPending) {
			finishIndexing(true);
		}
	});
	if (profile->getSettings()->value("recommendations/localEnabled", false).toBool() && modelAvailable()) {
		m_reindex->start();
	}
}

LibraryRecommendations::~LibraryRecommendations()
{
	blockSignals(true);
	disconnect(m_watcher, nullptr, this, nullptr);
	cancel();
	// The worker owns only a snapshot. Keep this callback target alive until its current inference ends.
	m_watcher->waitForFinished();
	if (m_indexPending) {
		finishIndexing(false);
	}
	delete m_download;
}

void LibraryRecommendations::finishIndexing(bool notify)
{
	m_indexPending = false;
	auto result = m_watcher->result();
	m_vectors = result.vectors;
	m_fingerprints = result.fingerprints;
	// A source link, deletion or preview update may have occurred during inference.
	invalidate(QString());
	m_reindex->stop();
	if (result.error.isEmpty()) {
		result.error = saveCache(cachePath(m_profile->getPath()), m_vectors, m_fingerprints);
	}
	if (!result.error.isEmpty()) {
		m_status = result.error;
	} else {
		m_profile->getSettings()->setValue("recommendations/localEnabled", true);
		m_status = tr("%1 indexed · %2 reused · %3 skipped (no usable preview) · %4 failed%5")
			.arg(result.indexed).arg(result.reused).arg(result.skipped).arg(result.failed)
			.arg(result.cancelled ? tr(" · Cancelled; completed indexing kept.") : QString());
	}
	if (!result.errors.isEmpty()) {
		m_status += " · " + result.errors.join("; ");
	}
	if (notify) {
		emit changed();
		emit indexFinished(result);
	}
	if (notify && m_dirty && !result.cancelled && result.error.isEmpty()) {
		m_dirty = false;
		m_reindex->start();
	}
}

QString LibraryRecommendations::cachePath(const QString &profileDirectory)
{
	return QDir(profileDirectory).filePath("recommendations/index.json");
}

bool LibraryRecommendations::busy() const
{
	return m_reply || m_watcher->isRunning();
}

bool LibraryRecommendations::modelAvailable() const
{
	const QFileInfo model(LibraryImageEncoder::modelPath(m_profile->getPath()));
	return model.isFile() && model.size() == LibraryImageEncoder::modelSize();
}

int LibraryRecommendations::indexedCount() const { return m_vectors.size(); }
QString LibraryRecommendations::status() const { return m_status; }

void LibraryRecommendations::loadCache()
{
	QFile file(cachePath(m_profile->getPath()));
	// ponytail: a bounded JSON cache covers personal libraries; use a derived SQLite table when indexes exceed 64 MiB.
	if (!file.open(QIODevice::ReadOnly) || file.size() > 64 * 1024 * 1024) {
		return;
	}
	const auto root = QJsonDocument::fromJson(file.readAll()).object();
	if (root["version"].toInt() != 1 || root["model"].toString() != LibraryImageEncoder::modelId()
		|| root["model_sha256"].toString() != LibraryImageEncoder::modelSha256()) {
		return;
	}
	const auto records = root["entries"].toObject();
	for (const auto &entry : m_profile->library()->entries()) {
		if (entry.thumbnail.isEmpty()) {
			continue;
		}
		const auto record = records[entry.key].toObject();
		const auto digest = record["thumbnail"].toString();
		if (digest != fingerprint(entry.thumbnail)) {
			continue;
		}
		const auto array = record["vector"].toArray();
		if (array.size() != 512) {
			continue;
		}
		QVector<float> vector;
		vector.reserve(512);
		bool numeric = true;
		for (const auto &value : array) {
			numeric = numeric && value.isDouble();
			vector.append(float(value.toDouble()));
		}
		if (numeric && validVector(vector)) {
			m_vectors.insert(entry.key, vector);
			m_fingerprints.insert(entry.key, digest);
		}
	}
}

void LibraryRecommendations::invalidate(const QString &key)
{
	bool needsIndex = false;
	if (key.isEmpty()) {
		QHash<QString, QString> current;
		for (const auto &entry : m_profile->library()->entries()) {
			if (!entry.thumbnail.isEmpty()) {
				current.insert(entry.key, fingerprint(entry.thumbnail));
				needsIndex = needsIndex || !m_vectors.contains(entry.key) || current.value(entry.key) != m_fingerprints.value(entry.key);
			}
		}
		for (const auto &cachedKey : m_vectors.keys()) {
			if (current.value(cachedKey) != m_fingerprints.value(cachedKey)) {
				m_vectors.remove(cachedKey); m_fingerprints.remove(cachedKey);
			}
		}
	} else {
		const auto entry = m_profile->library()->entry(key);
		needsIndex = !entry.thumbnail.isEmpty() && (!m_vectors.contains(key) || fingerprint(entry.thumbnail) != m_fingerprints.value(key));
		if (entry.key.isEmpty() || fingerprint(entry.thumbnail) != m_fingerprints.value(key)) {
			m_vectors.remove(key); m_fingerprints.remove(key);
		}
	}
	if (needsIndex && m_watcher->isRunning()) {
		m_dirty = true;
	} else if (needsIndex && modelAvailable() && m_profile->getSettings()->value("recommendations/localEnabled", false).toBool()) {
		m_reindex->start();
	}
	emit changed();
}

LibraryRecommendationResult LibraryRecommendations::recommendations(qint64 scope, const QDate &day, int limit)
{
	const auto candidates = m_profile->library()->entries();
	QHash<QString, QVector<float>> currentVectors;
	for (const auto &entry : candidates) {
		if (m_vectors.contains(entry.key) && !entry.thumbnail.isEmpty() && fingerprint(entry.thumbnail) == m_fingerprints.value(entry.key)) {
			currentVectors.insert(entry.key, m_vectors.value(entry.key));
		}
	}
	const auto seeds = scope > 0 ? m_profile->library()->entries(scope) : candidates;
	const auto hiddenList = m_profile->getSettings()->value(hiddenSetting(scope)).toStringList();
	return LibraryRecommender::rank(candidates, seeds, currentVectors, scope, day, limit, QSet<QString>(hiddenList.begin(), hiddenList.end()));
}

void LibraryRecommendations::startIndexing()
{
	if (busy()) {
		return;
	}
	m_reindex->stop();
	if (!m_profile->library()->isReady()) {
		m_status = m_profile->library()->lastError(); emit changed();
		LibraryIndexResult result; result.error = m_status; emit indexFinished(result); return;
	}
	const auto entries = m_profile->library()->entries();
	const auto directory = m_profile->getPath();
	m_cancel = std::make_shared<std::atomic_bool>(false);
	m_dirty = false;
	m_indexPending = true;
	m_status = tr("Indexing pictures on this PC…");
	auto cancelFlag = m_cancel;
	const auto vectors = m_vectors;
	const auto fingerprints = m_fingerprints;
	m_watcher->setFuture(QtConcurrent::run([this, entries, directory, cancelFlag, vectors, fingerprints]() {
		LibraryIndexResult result;
		result.total = int(entries.size());
		result.vectors = vectors;
		result.fingerprints = fingerprints;
		LibraryImageEncoder encoder;
		if (!encoder.open(LibraryImageEncoder::modelPath(directory), &result.error)) {
			return result;
		}
		int done = 0;
		for (const auto &entry : entries) {
			if (cancelFlag->load()) {
				result.cancelled = true; break;
			}
			const auto digest = fingerprint(entry.thumbnail);
			if (entry.thumbnail.isEmpty()) {
				++result.skipped;
			} else if (result.fingerprints.value(entry.key) == digest && validVector(result.vectors.value(entry.key))) {
				++result.reused;
			} else {
				QString error;
				const auto vector = encoder.encode(entry.thumbnail, &error);
				if (validVector(vector)) {
					result.vectors.insert(entry.key, vector); result.fingerprints.insert(entry.key, digest); ++result.indexed;
				} else {
					++result.failed;
					if (result.errors.size() < 5) {
						result.errors.append(entry.key + ": " + error);
					}
				}
			}
			++done;
			QMetaObject::invokeMethod(this, [this, done, total = result.total]() { emit progress(done, total); }, Qt::QueuedConnection);
		}
		return result;
	}));
	emit changed();
}

void LibraryRecommendations::cancel()
{
	m_reindex->stop();
	if (m_cancel) {
		m_cancel->store(true);
	}
	if (m_reply) {
		m_reply->abort();
	}
}

void LibraryRecommendations::hide(const QString &key, qint64 scope)
{
	auto keys = m_profile->getSettings()->value(hiddenSetting(scope)).toStringList();
	if (!keys.contains(key)) {
		keys.append(key);
	}
	m_profile->getSettings()->setValue(hiddenSetting(scope), keys);
	emit changed();
}

void LibraryRecommendations::restoreHidden(qint64 scope)
{
	m_profile->getSettings()->remove(hiddenSetting(scope)); emit changed();
}

void LibraryRecommendations::downloadModel()
{
	if (busy()) {
		return;
	}
	const auto path = LibraryImageEncoder::modelPath(m_profile->getPath());
	if (!ensureFileParent(path)) {
		m_status = tr("Could not create the local model folder."); emit changed(); return;
	}
	m_download = new QSaveFile(path);
	if (!m_download->open(QIODevice::WriteOnly)) {
		m_status = m_download->errorString(); delete m_download; m_download = nullptr; emit changed(); return;
	}
	auto *network = new QNetworkAccessManager(this);
	QNetworkRequest request {QUrl(LibraryImageEncoder::modelUrl())};
	request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
	request.setRawHeader("Accept-Encoding", "identity");
	request.setTransferTimeout(30000);
	m_reply = network->get(request);
	m_reply->setReadBufferSize(1024 * 1024);
	m_downloadBytes = 0;
	m_status = tr("Downloading the local model; no pictures are uploaded…");
	auto hash = std::make_shared<QCryptographicHash>(QCryptographicHash::Sha256);
	auto read = [this, hash]() {
		const auto bytes = m_reply->readAll();
		m_downloadBytes += bytes.size();
		if (m_downloadBytes > LibraryImageEncoder::modelSize() || m_download->write(bytes) != bytes.size()) {
			m_status = tr("Model download exceeded its size limit or could not be written.");
			m_reply->abort(); return;
		}
		hash->addData(bytes);
		emit progress(int(m_downloadBytes), int(LibraryImageEncoder::modelSize()));
	};
	connect(m_reply, &QNetworkReply::readyRead, this, read);
	connect(m_reply, &QNetworkReply::finished, this, [this, network, hash, read]() {
		read();
		const bool verified = m_reply->error() == QNetworkReply::NoError
			&& m_reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 200
			&& m_downloadBytes == LibraryImageEncoder::modelSize()
			&& QString::fromLatin1(hash->result().toHex()) == LibraryImageEncoder::modelSha256();
		const auto error = m_reply->errorString();
		const bool committed = verified && m_download->commit();
		if (!committed) {
			m_download->cancelWriting();
			m_status = tr("Model download was cancelled or failed verification: %1. Existing model was preserved.").arg(error);
		}
		m_reply->deleteLater(); m_reply = nullptr;
		network->deleteLater();
		delete m_download; m_download = nullptr;
		emit changed();
		if (committed) {
			startIndexing();
		}
	});
	QTimer::singleShot(10 * 60 * 1000, m_reply, [reply = m_reply]() {
		if (!reply->isFinished()) {
			reply->abort();
		}
	});
	emit changed();
}
