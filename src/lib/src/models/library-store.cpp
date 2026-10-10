#include "models/library-store.h"
#include <QBuffer>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QFile>
#include "utils/file-utils.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QPixmap>
#include <QRegularExpression>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUuid>
#include "models/image.h"
#include "models/library-importer.h"
#include "models/site.h"


QStringList LibraryEntry::tags() const
{
	QStringList result;
	const auto value = image.value("tags");
	if (value.isString()) {
		result = value.toString().split(' ', Qt::SkipEmptyParts);
	} else {
		for (const auto &tag : value.toArray()) {
			result.append(tag.isObject() ? tag.toObject().value("text").toString() : tag.toString());
		}
	}
	for (const auto &tag : image.value("local_import").toObject().value("tags").toArray()) {
		result.append(tag.toString());
	}
	result.removeAll(QString());
	result.removeDuplicates();
	return result;
}

QStringList LibraryEntry::metadataErrors() const
{
	QStringList result;
	for (const auto &item : image.value("local_import").toObject().value("evidence").toArray()) {
		const auto evidence = item.toObject();
		if (!evidence.value("error").toString().isEmpty()) {
			result.append(evidence.value("kind").toString() + ": " + evidence.value("error").toString());
		}
	}
	return result;
}

LibraryStore::LibraryStore(const QString &path, QObject *parent)
	: QObject(parent), m_connection("library-" + QUuid::createUuid().toString())
{
	m_directory = QFileInfo(path).absolutePath();
	if (path != ":memory:" && !QDir().mkpath(QFileInfo(path).absolutePath())) {
		m_error = tr("Cannot create the Library directory.");
		return;
	}
	connect(this, &LibraryStore::imageChanged, this, [this]() { m_ratedDirty = true; });
	connect(this, &LibraryStore::collectionsChanged, this, [this]() { m_ratedDirty = true; });
	m_database = QSqlDatabase::addDatabase("QSQLITE", m_connection);
	m_database.setDatabaseName(path);
	m_database.setConnectOptions("QSQLITE_BUSY_TIMEOUT=5000");
	if (!m_database.open()) {
		m_error = m_database.lastError().text();
		return;
	}
	QSqlQuery check(m_database);
	if (!check.exec("PRAGMA quick_check") || !check.next() || check.value(0).toString() != "ok") {
		m_error = tr("The Library database is damaged. Restore a backup before continuing.");
		return;
	}
	if (!execute("PRAGMA foreign_keys = ON") || !check.exec("PRAGMA user_version") || !check.next()) {
		m_error = m_database.lastError().text();
		return;
	}
	const int version = check.value(0).toInt();
	check.finish();
	if (version > 2 || version < 0) {
		m_error = tr("This Library was created by a newer Grabber. Open it with that version.");
		return;
	}
	if (version == 0) {
		if (!check.exec("SELECT name FROM sqlite_master WHERE type = 'table' AND name NOT LIKE 'sqlite_%'") || check.next()) {
			m_error = tr("Unrecognized Library database. The existing file has been preserved.");
			return;
		}
		check.finish();
		if (!m_database.transaction()) {
			m_error = m_database.lastError().text();
			return;
		}
		const QStringList schema {
			"CREATE TABLE images (key TEXT PRIMARY KEY, metadata TEXT NOT NULL, thumbnail BLOB, "
			"liked INTEGER NOT NULL DEFAULT 0 CHECK(liked IN (0,1)), favorite INTEGER NOT NULL DEFAULT 0 CHECK(favorite IN (0,1)), "
			"notes TEXT NOT NULL DEFAULT '', saved_at TEXT NOT NULL, updated_at TEXT NOT NULL)",
			"CREATE TABLE collections (id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT NOT NULL COLLATE NOCASE UNIQUE "
			"CHECK(length(trim(name)) > 0), cover_key TEXT REFERENCES images(key) ON DELETE SET NULL)",
			"CREATE TABLE members (image_key TEXT NOT NULL REFERENCES images(key) ON DELETE CASCADE, "
			"collection_id INTEGER NOT NULL REFERENCES collections(id) ON DELETE CASCADE, "
			"liked INTEGER NOT NULL DEFAULT 0 CHECK(liked IN (0,1)), favorite INTEGER NOT NULL DEFAULT 0 CHECK(favorite IN (0,1)), "
			"notes TEXT NOT NULL DEFAULT '', PRIMARY KEY(image_key,collection_id))",
			"CREATE INDEX members_collection ON members(collection_id)",
			"PRAGMA user_version = 1"
		};
		for (const QString &sql : schema) {
			if (!execute(sql)) {
				m_database.rollback();
				return;
			}
		}
		if (!m_database.commit()) {
			m_error = m_database.lastError().text();
			m_database.rollback();
			return;
		}
	}
	// Validate the original schema before backing it up or attempting migration.
	if (!execute("SELECT key,metadata,thumbnail,liked,favorite,notes,saved_at,updated_at FROM images LIMIT 0")
		|| !execute("SELECT id,name,cover_key FROM collections LIMIT 0")
		|| !execute("SELECT image_key,collection_id,liked,favorite,notes FROM members LIMIT 0")) {
		return;
	}
	if (version < 2) {
		if (version == 1 && path != ":memory:") {
			const QString backup = path + ".before-import-" + QDateTime::currentDateTimeUtc().toString("yyyyMMdd-HHmmss-zzz") + "-" + QUuid::createUuid().toString(QUuid::Id128) + ".bak";
			if (!execute("VACUUM INTO ?", { backup })) {
				m_error = tr("Cannot back up the Library before upgrading: %1").arg(m_error);
				return;
			}
		}
		if (!m_database.transaction()) {
			m_error = m_database.lastError().text();
			return;
		}
		const QStringList schema {
			"CREATE TABLE local_files (path TEXT PRIMARY KEY, image_key TEXT NOT NULL REFERENCES images(key) ON DELETE CASCADE, "
			"sha256 TEXT NOT NULL, md5 TEXT NOT NULL, source_md5 TEXT NOT NULL, visual_hash TEXT NOT NULL)",
			"CREATE INDEX local_files_hash ON local_files(sha256)",
			"CREATE INDEX local_files_image ON local_files(image_key)",
			"CREATE TABLE source_links (source_key TEXT PRIMARY KEY, image_key TEXT NOT NULL REFERENCES images(key) ON DELETE CASCADE)",
			"PRAGMA user_version = 2"
		};
		for (const QString &sql : schema) {
			if (!execute(sql)) {
				m_database.rollback();
				return;
			}
		}
		if (!m_database.commit()) {
			m_error = m_database.lastError().text();
			m_database.rollback();
			return;
		}
	}
	if (!execute("SELECT path,image_key,sha256,md5,source_md5,visual_hash FROM local_files LIMIT 0")
		|| !execute("SELECT source_key,image_key FROM source_links LIMIT 0")) {
		return;
	}
	if (!check.exec("SELECT 1 FROM images WHERE liked=1 AND favorite=1 UNION ALL SELECT 1 FROM members WHERE liked=1 AND favorite=1 LIMIT 1")) {
		m_error = check.lastError().text();
		return;
	}
	const bool overlappingRatings = check.next();
	check.finish();
	if (overlappingRatings) {
		// Preserve legacy choices before reducing overlapping ratings to the stronger favorite.
		if (path != ":memory:") {
			const QString backup = path + ".before-exclusive-ratings-" + QDateTime::currentDateTimeUtc().toString("yyyyMMdd-HHmmss-zzz") + "-" + QUuid::createUuid().toString(QUuid::Id128) + ".bak";
			if (!execute("VACUUM INTO ?", {backup})) {
				m_error = tr("Cannot back up the Library before updating ratings: %1").arg(m_error);
				return;
			}
		}
		if (!m_database.transaction()) {
			m_error = m_database.lastError().text();
			return;
		}
		const bool ok = execute("UPDATE images SET liked=0 WHERE liked=1 AND favorite=1")
			&& execute("UPDATE members SET liked=0 WHERE liked=1 AND favorite=1");
		if (!ok || !m_database.commit()) {
			if (ok) {
				m_error = m_database.lastError().text();
			}
			m_database.rollback();
			return;
		}
	}
	m_ready = true;
}

LibraryStore::~LibraryStore()
{
	m_database.close();
	m_database = QSqlDatabase();
	QSqlDatabase::removeDatabase(m_connection);
}

bool LibraryStore::isReady() const { return m_ready; }
QString LibraryStore::lastError() const { return m_error; }

bool LibraryStore::execute(const QString &sql, const QVariantList &values)
{
	QSqlQuery query(m_database);
	query.prepare(sql);
	for (const auto &value : values) {
		query.addBindValue(value);
	}
	if (!query.exec()) {
		m_error = query.lastError().text();
		return false;
	}
	return true;
}

QString LibraryStore::imageKey(const Image &image)
{
	if (image.parentSite() == nullptr) {
		return {};
	}
	QVariantMap identity = image.identity();
	if (identity.contains("id")) {
		identity["id"] = identity["id"].toString();
	}
	if (identity.isEmpty() && image.id() != 0) {
		identity.insert("id", QString::number(image.id()));
	}
	if (identity.isEmpty()) {
		if (!image.md5().isEmpty()) {
			identity.insert("md5", image.md5());
		} else {
			identity.insert("file", image.fileUrl().toString(QUrl::RemoveQuery | QUrl::RemoveFragment));
			if (!image.pageUrl().isEmpty()) {
				identity.insert("page", image.pageUrl().toString(QUrl::RemoveFragment));
			}
		}
	}
	if (identity.value("file").toString().isEmpty() && identity.size() == 1 && identity.contains("file")) {
		return {};
	}
	QJsonObject json;
	image.write(json);
	if (json.contains("gallery")) {
		// Attachment identities can equal their parent's post ID. Keep siblings separate.
		identity.insert("attachment", image.fileUrl().path());
		identity.insert("gallery", json.value("gallery").toObject().value("identity").toObject().toVariantMap());
	}
	const QJsonArray qualified { image.parentSite()->url().toLower(), QJsonObject::fromVariantMap(identity) };
	return QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(qualified).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex());
}

QString LibraryStore::saveImage(const Image &image)
{
	if (!m_ready) {
		return {};
	}
	const QString key = keyForImage(image);
	if (key.isEmpty()) {
		m_error = tr("This picture has no usable source identity.");
		return {};
	}
	QJsonObject metadata;
	image.write(metadata);
	const auto previous = entry(key).image;
	for (const QString &field : {QString("local_import"), QString("source_link_evidence")}) {
		if (previous.contains(field)) {
			metadata.insert(field, previous.value(field));
		}
	}
	QByteArray thumbnail;
	const QPixmap preview = image.previewImage();
	if (!preview.isNull()) {
		metadata.insert("visual_hash", LibraryImporter::visualHash(preview.toImage()));
	}
	if (!preview.isNull()) {
		QBuffer buffer(&thumbnail);
		buffer.open(QIODevice::WriteOnly);
		preview.scaled(384, 384, Qt::KeepAspectRatio, Qt::SmoothTransformation).save(&buffer, "PNG");
	}
	const QString now = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
	if (!execute("INSERT INTO images (key,metadata,thumbnail,saved_at,updated_at) VALUES (?,?,?,?,?) "
		"ON CONFLICT(key) DO UPDATE SET metadata=excluded.metadata, thumbnail=coalesce(excluded.thumbnail,images.thumbnail), updated_at=excluded.updated_at",
		{ key, QString::fromUtf8(QJsonDocument(metadata).toJson(QJsonDocument::Compact)), thumbnail.isEmpty() ? QVariant() : QVariant(thumbnail), now, now })) {
		return {};
	}
	emit imageChanged(key);
	return key;
}

QList<LibraryEntry> LibraryStore::entries(qint64 collection)
{
	QList<LibraryEntry> result;
	if (!m_ready) {
		return result;
	}
	QSqlQuery query(m_database);
	const QString scope = collection > 0 ? "m" : "i";
	QString sql = "SELECT i.key,i.metadata,i.thumbnail," + scope + ".liked," + scope + ".favorite," + scope + ".notes,i.saved_at,"
		"(SELECT count(*) FROM members WHERE image_key=i.key) FROM images i";
	if (collection > 0) {
		sql += " JOIN members m ON m.image_key=i.key WHERE m.collection_id=?";
	}
	sql += " ORDER BY i.saved_at DESC,i.key";
	query.prepare(sql);
	if (collection > 0) {
		query.addBindValue(collection);
	}
	if (!query.exec()) {
		m_error = query.lastError().text();
		return result;
	}
	while (query.next()) {
		result.append({ query.value(0).toString(), QJsonDocument::fromJson(query.value(1).toByteArray()).object(),
						query.value(2).toByteArray(), query.value(3).toBool(), query.value(4).toBool(), query.value(5).toString(), query.value(6).toString(), query.value(7).toInt() });
	}
	query.finish();
	QHash<QString, int> indexes;
	for (int i = 0; i < result.size(); ++i) {
		indexes.insert(result[i].key, i);
	}
	if (query.exec("SELECT image_key,path FROM local_files ORDER BY path")) {
		while (query.next()) {
			const auto it = indexes.constFind(query.value(0).toString());
			if (it != indexes.cend()) {
				result[*it].localPaths.append(resolvedPath(query.value(1).toString()));
			}
		}
	} else {
		m_error = query.lastError().text();
	}
	return result;
}

LibraryEntry LibraryStore::entry(const QString &key, qint64 collection)
{
	LibraryEntry result;
	if (!m_ready) {
		return result;
	}
	const QString scope = collection > 0 ? "m" : "i";
	QSqlQuery query(m_database);
	query.prepare("SELECT i.metadata,i.thumbnail," + scope + ".liked," + scope + ".favorite," + scope + ".notes,i.saved_at,"
		"(SELECT count(*) FROM members WHERE image_key=i.key) FROM images i "
		+ (collection > 0 ? QString("JOIN members m ON m.image_key=i.key AND m.collection_id=? ") : QString()) + "WHERE i.key=?");
	if (collection > 0) {
		query.addBindValue(collection);
	}
	query.addBindValue(key);
	if (query.exec() && query.next()) {
		result = { key, QJsonDocument::fromJson(query.value(0).toByteArray()).object(), query.value(1).toByteArray(), query.value(2).toBool(),
				   query.value(3).toBool(), query.value(4).toString(), query.value(5).toString(), query.value(6).toInt() };
	} else if (query.lastError().isValid()) {
		m_error = query.lastError().text();
	}
	query.finish();
	if (!result.key.isEmpty()) {
		query.prepare("SELECT path FROM local_files WHERE image_key=? ORDER BY path");
		query.addBindValue(key);
		if (query.exec()) {
			while (query.next()) {
				result.localPaths.append(resolvedPath(query.value(0).toString()));
			}
		} else {
			m_error = query.lastError().text();
		}
	}
	return result;
}

bool LibraryStore::contains(const QString &key, qint64 collection)
{
	if (!m_ready) {
		return false;
	}
	QSqlQuery query(m_database);
	query.prepare(collection > 0 ? "SELECT 1 FROM members WHERE image_key=? AND collection_id=?" : "SELECT 1 FROM images WHERE key=?");
	query.addBindValue(key);
	if (collection > 0) {
		query.addBindValue(collection);
	}
	if (!query.exec()) {
		m_error = query.lastError().text();
		return false;
	}
	return query.next();
}

bool LibraryStore::setValue(const QString &key, const QString &column, const QVariant &value, qint64 collection)
{
	if (!m_ready || !contains(key, collection)) {
		if (m_ready) {
			m_error = tr("The picture is no longer in this Library view.");
		}
		return false;
	}
	QString assignment = column + "=?";
	if (value.toBool() && (column == "liked" || column == "favorite")) {
		assignment += column == "liked" ? ",favorite=0" : ",liked=0";
	}
	const bool ok = collection > 0
		? execute("UPDATE members SET " + assignment + " WHERE image_key=? AND collection_id=?", { value, key, collection })
		: execute("UPDATE images SET " + assignment + " WHERE key=?", { value, key });
	if (ok) {
		emit imageChanged(key);
	}
	return ok;
}

bool LibraryStore::setLiked(const QString &key, bool value, qint64 collection) { return setValue(key, "liked", value, collection); }
bool LibraryStore::setFavorite(const QString &key, bool value, qint64 collection) { return setValue(key, "favorite", value, collection); }
bool LibraryStore::setNotes(const QString &key, const QString &value, qint64 collection) { return setValue(key, "notes", value, collection); }

bool LibraryStore::removeImage(const QString &key)
{
	if (!m_ready || !execute("DELETE FROM images WHERE key=?", { key })) {
		return false;
	}
	emit imageChanged(key);
	emit collectionsChanged();
	return true;
}

QList<LibraryCollection> LibraryStore::collections()
{
	QList<LibraryCollection> result;
	if (!m_ready) {
		return result;
	}
	QSqlQuery query(m_database);
	if (!query.exec("SELECT c.id,c.name,(SELECT count(*) FROM members WHERE collection_id=c.id),"
		"coalesce((SELECT i.thumbnail FROM images i JOIN members m ON i.key=m.image_key WHERE m.collection_id=c.id AND i.key=c.cover_key),"
		"(SELECT i.thumbnail FROM images i JOIN members m ON i.key=m.image_key WHERE m.collection_id=c.id ORDER BY m.favorite DESC,i.saved_at DESC LIMIT 1)) "
		"FROM collections c ORDER BY c.name COLLATE NOCASE,c.id")) {
		m_error = query.lastError().text();
		return result;
	}
	while (query.next()) {
		result.append({ query.value(0).toLongLong(), query.value(1).toString(), query.value(2).toInt(), query.value(3).toByteArray() });
	}
	return result;
}

qint64 LibraryStore::createCollection(const QString &name)
{
	if (!m_ready || name.trimmed().isEmpty()) {
		if (m_ready) {
			m_error = tr("Enter a collection name.");
		}
		return 0;
	}
	QSqlQuery query(m_database);
	query.prepare("INSERT INTO collections (name) VALUES (?)");
	query.addBindValue(name.trimmed());
	if (!query.exec()) {
		m_error = query.lastError().text();
		return 0;
	}
	const qint64 id = query.lastInsertId().toLongLong();
	emit collectionsChanged();
	return id;
}

bool LibraryStore::renameCollection(qint64 id, const QString &name)
{
	if (!m_ready || name.trimmed().isEmpty()) {
		if (m_ready) {
			m_error = tr("Enter a collection name.");
		}
		return false;
	}
	if (!execute("UPDATE collections SET name=? WHERE id=?", { name.trimmed(), id })) {
		return false;
	}
	emit collectionsChanged();
	return true;
}

bool LibraryStore::removeCollection(qint64 id)
{
	if (!m_ready || !execute("DELETE FROM collections WHERE id=?", { id })) {
		return false;
	}
	emit collectionsChanged();
	// Membership and scope-specific preferences were removed together by SQLite.
	emit imageChanged(QString());
	return true;
}

bool LibraryStore::setCollectionCover(qint64 id, const QString &key)
{
	if (!m_ready || !contains(key, id) || !execute("UPDATE collections SET cover_key=? WHERE id=?", { key, id })) {
		return false;
	}
	emit collectionsChanged();
	return true;
}

bool LibraryStore::addToCollection(const QString &key, qint64 collection)
{
	if (!m_ready || !execute("INSERT INTO members (image_key,collection_id) VALUES (?,?) ON CONFLICT DO NOTHING", { key, collection })) {
		return false;
	}
	emit imageChanged(key);
	emit collectionsChanged();
	return true;
}

bool LibraryStore::removeFromCollection(const QString &key, qint64 collection)
{
	if (!m_ready || !execute("DELETE FROM members WHERE image_key=? AND collection_id=?", { key, collection })) {
		return false;
	}
	emit imageChanged(key);
	emit collectionsChanged();
	return true;
}

QString LibraryStore::storedPath(const QString &path) const
{
	const QString absolute = QFileInfo(path).absoluteFilePath();
	const QString relative = QDir(m_directory).relativeFilePath(absolute);
	return relative.startsWith("library-media/") ? relative : absolute;
}

QString LibraryStore::resolvedPath(const QString &path) const
{
	return QDir::isAbsolutePath(path) ? path : QDir(m_directory).absoluteFilePath(path);
}

void LibraryStore::loadRatedIndex()
{
	m_ratedKeys.clear();
	m_ratedMd5s.clear();
	m_ratedDirty = false;
	if (!m_ready) {
		return;
	}
	static const QRegularExpression md5("^[0-9a-fA-F]{32}$");
	QSqlQuery query(m_database);
	const QString rated = "SELECT key FROM images WHERE liked=1 OR favorite=1 UNION SELECT image_key FROM members WHERE liked=1 OR favorite=1";
	if (!query.exec("SELECT i.key,i.metadata FROM images i WHERE i.key IN (" + rated + ")")) {
		m_error = query.lastError().text();
		return;
	}
	while (query.next()) {
		m_ratedKeys.insert(query.value(0).toString());
		const QString hash = QJsonDocument::fromJson(query.value(1).toByteArray()).object().value("md5").toString();
		if (md5.match(hash).hasMatch()) {
			m_ratedMd5s.insert(hash.toLower());
		}
	}
	if (query.exec("SELECT source_key FROM source_links WHERE image_key IN (" + rated + ")")) {
		while (query.next()) {
			m_ratedKeys.insert(query.value(0).toString());
		}
	}
	if (query.exec("SELECT md5,source_md5 FROM local_files WHERE image_key IN (" + rated + ")")) {
		while (query.next()) {
			for (int column = 0; column < 2; ++column) {
				const QString hash = query.value(column).toString();
				if (md5.match(hash).hasMatch()) {
					m_ratedMd5s.insert(hash.toLower());
				}
			}
		}
	}
}

bool LibraryStore::isRated(const Image &image)
{
	if (m_ratedDirty) {
		loadRatedIndex();
	}
	if (m_ratedKeys.isEmpty() && m_ratedMd5s.isEmpty()) {
		return false;
	}
	const QString md5 = image.md5().toLower();
	return (!md5.isEmpty() && m_ratedMd5s.contains(md5)) || m_ratedKeys.contains(imageKey(image));
}

QString LibraryStore::keyForImage(const Image &image)
{
	const QString source = imageKey(image);
	if (!m_ready || source.isEmpty()) {
		return source;
	}
	QSqlQuery query(m_database);
	query.prepare("SELECT image_key FROM source_links WHERE source_key=?");
	query.addBindValue(source);
	if (!query.exec()) {
		m_error = query.lastError().text(); return {};
	}
	return query.next() ? query.value(0).toString() : source;
}

QString LibraryStore::saveLocalImage(const LibraryImportData &data, qint64 collection, const QString &expectedKey)
{
	static const QRegularExpression sha("^[0-9a-f]{64}$"), md5("^[0-9a-f]{32}$");
	if (!m_ready || !data.error.isEmpty() || !sha.match(data.sha256).hasMatch() || !md5.match(data.md5).hasMatch()
		|| data.path.isEmpty() || !data.size.isValid() || (data.thumbnail.isEmpty() && data.metadata.value("preview_error").toString().isEmpty())) {
		m_error = data.error.isEmpty() ? tr("This file has no usable image or content hash.") : data.error;
		return {};
	}
	QString key;
	QSqlQuery query(m_database);
	query.prepare("SELECT image_key FROM local_files WHERE sha256=? LIMIT 1");
	query.addBindValue(data.sha256);
	if (!query.exec()) {
		m_error = query.lastError().text(); return {};
	}
	if (query.next()) {
		key = query.value(0).toString();
	}
	query.finish();
	if (!expectedKey.isEmpty()) {
		const auto expected = entry(expectedKey);
		const QString expectedSha = expected.image.value("local_import").toObject().value("sha256").toString();
		if (expected.key.isEmpty() || (!expectedSha.isEmpty() ? expectedSha != data.sha256 : expected.image.value("md5").toString().toLower() != data.md5)) {
			m_error = tr("The selected file does not match this picture's recorded content hash.");
			return {};
		}
		if (!key.isEmpty() && key != expectedKey) {
			m_error = tr("This file already belongs to another Library picture. Review its source link first.");
			return {};
		}
		key = expectedKey;
	}
	// Only a verified byte hash can attach to an existing remote image automatically.
	if (key.isEmpty()) {
		QStringList exact;
		query.prepare("SELECT key,metadata FROM images WHERE metadata LIKE ?");
		query.addBindValue("%" + data.md5 + "%");
		if (!query.exec()) {
			m_error = query.lastError().text(); return {};
		}
		while (query.next()) {
			const auto item = QJsonDocument::fromJson(query.value(1).toByteArray()).object();
			if (!item.value("website").toString().isEmpty() && item.value("md5").toString().toLower() == data.md5) {
				exact.append(query.value(0).toString());
			}
		}
		query.finish();
		if (exact.size() == 1) {
			key = exact.first();
		}
	}
	if (key.isEmpty()) {
		key = "local-" + data.sha256;
	}
	QJsonObject metadata = entry(key).image;
	if (metadata.isEmpty()) {
		metadata.insert("name", data.title);
		metadata.insert("tags", data.tags.join(' '));
	}
	QJsonObject local = metadata.value("local_import").toObject();
	if (local.isEmpty()) {
		local = data.metadata;
		local.insert("sha256", data.sha256);
		local.insert("md5", data.md5);
		local.insert("source_md5", data.sourceMd5);
		local.insert("visual_hash", data.visualHash);
		local.insert("width", data.size.width());
		local.insert("height", data.size.height());
		local.insert("source_urls", QJsonArray::fromStringList(data.sourceUrls));
		local.insert("tags", QJsonArray::fromStringList(data.tags));
	} else {
		QJsonArray evidenceItems = local.value("evidence").toArray();
		for (const auto &item : data.metadata.value("evidence").toArray()) {
			const auto incoming = item.toObject();
			for (int index = evidenceItems.size() - 1; index >= 0; --index) {
				const auto previous = evidenceItems[index].toObject();
				if (previous.value("kind") == incoming.value("kind") && previous.value("path") == incoming.value("path")) {
					evidenceItems.removeAt(index);
				}
			}
			evidenceItems.append(item);
		}
		local.insert("evidence", evidenceItems);
		for (const QString &field : {QString("source_urls"), QString("tags")}) {
			QJsonArray values = local.value(field).toArray();
			const auto incoming = QJsonArray::fromStringList(field == "tags" ? data.tags : data.sourceUrls);
			for (const auto &value : incoming) {
				if (!values.contains(value)) {
					values.append(value);
				}
			}
			local.insert(field, values);
		}
		if (local.value("source_md5").toString().isEmpty()) {
			local.insert("source_md5", data.sourceMd5);
		}
	}
	if (metadata.value("website").toString().isEmpty()) {
		QStringList tags;
		for (const auto &tag : local.value("tags").toArray()) {
			tags.append(tag.toString());
		}
		metadata.insert("tags", tags.join(' '));
	}
	local.insert("preview_error", data.metadata.value("preview_error"));
	local.insert("extended_reader", data.metadata.value("extended_reader"));
	metadata.insert("local_import", local);
	const QString now = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
	if (!m_database.transaction()) {
		m_error = m_database.lastError().text(); return {};
	}
	const bool ok = execute("INSERT INTO images(key,metadata,thumbnail,saved_at,updated_at) VALUES(?,?,?,?,?) "
		"ON CONFLICT(key) DO UPDATE SET metadata=excluded.metadata,thumbnail=coalesce(images.thumbnail,excluded.thumbnail),updated_at=excluded.updated_at",
		{key, QString::fromUtf8(QJsonDocument(metadata).toJson(QJsonDocument::Compact)), data.thumbnail, now, now})
		&& execute("INSERT INTO local_files(path,image_key,sha256,md5,source_md5,visual_hash) VALUES(?,?,?,?,?,?) "
		"ON CONFLICT(path) DO UPDATE SET image_key=excluded.image_key,sha256=excluded.sha256,md5=excluded.md5,source_md5=excluded.source_md5,visual_hash=excluded.visual_hash",
		{storedPath(data.path), key, data.sha256, data.md5, data.sourceMd5.isEmpty() ? QStringLiteral("") : data.sourceMd5, data.visualHash.isEmpty() ? QStringLiteral("") : data.visualHash})
		&& (collection <= 0 || execute("INSERT INTO members(image_key,collection_id) VALUES(?,?) ON CONFLICT DO NOTHING", {key, collection}));
	if (!ok || !m_database.commit()) {
		if (ok) {
			m_error = m_database.lastError().text();
		}
		m_database.rollback();
		return {};
	}
	emit imageChanged(key);
	if (collection > 0) {
		emit collectionsChanged();
	}
	return key;
}

bool LibraryStore::linkSource(const QString &key, const Image &image, const QString &evidence)
{
	const auto target = entry(key);
	const QString source = imageKey(image);
	const QString other = keyForImage(image);
	if (!m_ready || target.key.isEmpty() || source.isEmpty()) {
		m_error = tr("The picture or source identity is unavailable."); return false;
	}
	QJsonObject metadata;
	image.write(metadata);
	QJsonObject local = target.image.value("local_import").toObject();
	const QJsonObject otherLocal = entry(other).image.value("local_import").toObject();
	if (local.isEmpty()) {
		local = otherLocal;
	} else if (!otherLocal.isEmpty() && other != key) {
		QJsonArray evidenceItems = local.value("evidence").toArray();
		for (const auto &item : otherLocal.value("evidence").toArray()) {
			if (!evidenceItems.contains(item)) {
				evidenceItems.append(item);
			}
		}
		local.insert("evidence", evidenceItems);
		for (const QString &field : {QString("source_urls"), QString("tags")}) {
			QJsonArray values = local.value(field).toArray();
			for (const auto &value : otherLocal.value(field).toArray()) {
				if (!values.contains(value)) {
					values.append(value);
				}
			}
			local.insert(field, values);
		}
	}
	if (!local.isEmpty()) {
		metadata.insert("local_import", local);
	}
	metadata.insert("source_link_evidence", evidence);
	if (!m_database.transaction()) {
		m_error = m_database.lastError().text(); return false;
	}
	bool ok = true;
	if (other != key && contains(other)) {
		ok = execute("UPDATE images SET liked=CASE WHEN max(favorite,(SELECT favorite FROM images WHERE key=?))=1 THEN 0 ELSE max(liked,(SELECT liked FROM images WHERE key=?)) END, "
			"favorite=max(favorite,(SELECT favorite FROM images WHERE key=?)), "
			"notes=CASE WHEN notes='' THEN (SELECT notes FROM images WHERE key=?) WHEN (SELECT notes FROM images WHERE key=?)='' OR notes=(SELECT notes FROM images WHERE key=?) THEN notes "
			"ELSE notes || char(10) || (SELECT notes FROM images WHERE key=?) END WHERE key=?", {other, other, other, other, other, other, other, key})
			&& execute("INSERT INTO members(image_key,collection_id,liked,favorite,notes) SELECT ?,collection_id,CASE WHEN favorite=1 THEN 0 ELSE liked END,favorite,notes FROM members WHERE image_key=? "
			"ON CONFLICT(image_key,collection_id) DO UPDATE SET liked=CASE WHEN max(members.favorite,excluded.favorite)=1 THEN 0 ELSE max(members.liked,excluded.liked) END, "
			"favorite=max(members.favorite,excluded.favorite), "
			"notes=CASE WHEN members.notes='' THEN excluded.notes WHEN excluded.notes='' OR members.notes=excluded.notes THEN members.notes ELSE members.notes || char(10) || excluded.notes END", {key, other})
			&& execute("UPDATE collections SET cover_key=? WHERE cover_key=?", {key, other})
			&& execute("UPDATE local_files SET image_key=? WHERE image_key=?", {key, other})
			&& execute("UPDATE source_links SET image_key=? WHERE image_key=?", {key, other})
			&& execute("DELETE FROM images WHERE key=?", {other});
	}
	ok = ok && execute("UPDATE images SET metadata=?,updated_at=? WHERE key=?",
		{QString::fromUtf8(QJsonDocument(metadata).toJson(QJsonDocument::Compact)), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs), key})
		&& execute("INSERT INTO source_links(source_key,image_key) VALUES(?,?) ON CONFLICT(source_key) DO UPDATE SET image_key=excluded.image_key", {source, key});
	if (!ok || !m_database.commit()) {
		if (ok) {
			m_error = m_database.lastError().text();
		}
		m_database.rollback(); return false;
	}
	emit imageChanged(QString());
	emit collectionsChanged();
	return true;
}

bool LibraryStore::backupTo(const QString &path)
{
	if (!m_ready || !QDir().mkpath(QFileInfo(path).absolutePath())) {
		if (m_ready) {
			m_error = tr("Cannot prepare the Library backup directory.");
		}
		return false;
	}
	return execute("VACUUM INTO ?", {QFileInfo(path).absoluteFilePath()});
}

bool LibraryStore::restoreFrom(const QString &path)
{
	const QFileInfo incoming(path);
	if (!incoming.isFile()) {
		m_error = tr("The Library backup file is missing.");
		return false;
	}
	if (incoming.canonicalFilePath() == QFileInfo(m_database.databaseName()).canonicalFilePath()) {
		return true;
	}
	QTemporaryDir validationDirectory;
	const QString validated = validationDirectory.filePath("library.sqlite");
	if (!validationDirectory.isValid() || !QFile::copy(incoming.absoluteFilePath(), validated)) {
		m_error = tr("Cannot prepare a copy of this Library backup for validation.");
		return false;
	}
	{
		LibraryStore validation(validated);
		if (!validation.isReady()) {
			m_error = tr("Cannot restore this Library: %1").arg(validation.lastError());
			return false;
		}
	}
	const QString suffix = ".before-restore-" + QDateTime::currentDateTimeUtc().toString("yyyyMMdd-HHmmss-zzz") + "-" + QUuid::createUuid().toString(QUuid::Id128) + ".bak";
	const QString backup = m_database.databaseName() == ":memory:" ? incoming.absoluteFilePath() + suffix : m_database.databaseName() + suffix;
	if (!m_ready) {
		const QString target = m_database.databaseName();
		if (target.isEmpty() || target == ":memory:") {
			m_error = tr("The Library has no usable restore location."); return false;
		}
		m_database.close();
		if ((QFileInfo::exists(target) && !QFile::copy(target, backup)) || !atomicCopyFile(validated, target)) {
			m_database.open();
			m_error = tr("Cannot preserve or replace the damaged Library.");
			return false;
		}
		m_ready = m_database.open() && execute("PRAGMA foreign_keys=ON")
			&& execute("SELECT key,metadata,thumbnail,liked,favorite,notes FROM images LIMIT 0")
			&& execute("SELECT image_key,collection_id FROM members LIMIT 0")
			&& execute("SELECT path,image_key FROM local_files LIMIT 0") && execute("SELECT source_key,image_key FROM source_links LIMIT 0");
		if (m_ready) {
			m_error.clear(); emit imageChanged(QString()); emit collectionsChanged();
		}
		return m_ready;
	}
	if (!backupTo(backup) || !execute("ATTACH DATABASE ? AS library_restore", {validated})) {
		return false;
	}
	if (!m_database.transaction()) {
		m_error = m_database.lastError().text();
		const QString error = m_error;
		execute("DETACH DATABASE library_restore");
		m_error = error;
		return false;
	}
	const QStringList statements {
		"DELETE FROM members", "DELETE FROM source_links", "DELETE FROM local_files", "DELETE FROM collections", "DELETE FROM images",
		"INSERT INTO images(key,metadata,thumbnail,liked,favorite,notes,saved_at,updated_at) SELECT key,metadata,thumbnail,liked,favorite,notes,saved_at,updated_at FROM library_restore.images",
		"INSERT INTO collections(id,name,cover_key) SELECT id,name,cover_key FROM library_restore.collections",
		"INSERT INTO members(image_key,collection_id,liked,favorite,notes) SELECT image_key,collection_id,liked,favorite,notes FROM library_restore.members",
		"INSERT INTO local_files(path,image_key,sha256,md5,source_md5,visual_hash) SELECT path,image_key,sha256,md5,source_md5,visual_hash FROM library_restore.local_files",
		"INSERT INTO source_links(source_key,image_key) SELECT source_key,image_key FROM library_restore.source_links"
	};
	bool ok = true;
	for (const QString &sql : statements) {
		if (!execute(sql)) {
			ok = false; break;
		}
	}
	if (!ok || !m_database.commit()) {
		if (ok) {
			m_error = m_database.lastError().text();
		}
		const QString error = m_error;
		m_database.rollback();
		execute("DETACH DATABASE library_restore");
		m_error = error;
		return false;
	}
	if (!execute("DETACH DATABASE library_restore")) {
		return false;
	}
	emit imageChanged(QString());
	emit collectionsChanged();
	return true;
}
