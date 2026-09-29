#include "models/library-store.h"
#include <QBuffer>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPixmap>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>
#include "models/image.h"
#include "models/site.h"


LibraryStore::LibraryStore(const QString &path, QObject *parent)
	: QObject(parent), m_connection("library-" + QUuid::createUuid().toString())
{
	if (path != ":memory:" && !QDir().mkpath(QFileInfo(path).absolutePath())) {
		m_error = tr("Cannot create the Library directory.");
		return;
	}
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
	if (version > 1 || version < 0) {
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
	// Validate every table before enabling writes; never recreate an existing schema.
	m_ready = execute("SELECT key,metadata,thumbnail,liked,favorite,notes,saved_at,updated_at FROM images LIMIT 0")
		&& execute("SELECT id,name,cover_key FROM collections LIMIT 0")
		&& execute("SELECT image_key,collection_id,liked,favorite,notes FROM members LIMIT 0");
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
	const QString key = imageKey(image);
	if (key.isEmpty()) {
		m_error = tr("This picture has no usable source identity.");
		return {};
	}
	QJsonObject metadata;
	image.write(metadata);
	QByteArray thumbnail;
	const QPixmap preview = image.previewImage();
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
	const bool ok = collection > 0
		? execute("UPDATE members SET " + column + "=? WHERE image_key=? AND collection_id=?", { value, key, collection })
		: execute("UPDATE images SET " + column + "=? WHERE key=?", { value, key });
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
