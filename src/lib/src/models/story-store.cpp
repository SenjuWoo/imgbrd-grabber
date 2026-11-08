#include "models/story-store.h"
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include "logger.h"
#include "utils/file-utils.h"


namespace
{
	// Far above any real store (a few hundred bytes per story); only a corrupt or foreign file reaches it.
	constexpr qint64 MaxFileSize = 256 * 1024 * 1024;
}

StoryStore::StoryStore(const QString &profileDirectory)
	: m_path(QDir(profileDirectory).filePath("stories.json"))
{
}

// A file we cannot read is kept next to the new one instead of being overwritten, so a damaged store
// can still be recovered by hand. The caller closes the file first: Windows refuses to rename an open one.
bool StoryStore::keepUnreadable() const
{
	const QString backup = m_path + QStringLiteral(".corrupt-") + QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd-HHmmss"));
	if (QFile::rename(m_path, backup)) {
		return true;
	}
	log(QStringLiteral("Stories: could not move `%1` aside; it will not be overwritten.").arg(m_path), Logger::Error);
	return false;
}

QString StoryStore::story(const QString &key) const
{
	load();
	return m_stories.value(key).text;
}

QString StoryStore::model(const QString &key) const
{
	load();
	return m_stories.value(key).model;
}

bool StoryStore::contains(const QString &key) const
{
	load();
	return m_stories.contains(key);
}

QStringList StoryStore::keys() const
{
	load();
	return m_stories.keys();
}

int StoryStore::count() const
{
	load();
	return int(m_stories.size());
}

void StoryStore::setStory(const QString &key, const QString &text, const QString &model)
{
	if (key.isEmpty() || text.trimmed().isEmpty()) {
		return;
	}
	load();
	m_stories.insert(key, {text.trimmed(), model, QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)});
	save();
}

void StoryStore::remove(const QString &key)
{
	load();
	if (m_stories.remove(key) > 0) {
		save();
	}
}

void StoryStore::load() const
{
	if (m_loaded) {
		return;
	}
	m_loaded = true;
	QFile file(m_path);
	if (!file.exists()) {
		return;
	}
	if (!file.open(QIODevice::ReadOnly)) {
		// We cannot see what is in there, so we must not write over it either.
		m_saveBlocked = true;
		log(QStringLiteral("Stories: `%1` cannot be read; it will not be overwritten.").arg(m_path), Logger::Error);
		return;
	}
	if (file.size() > MaxFileSize) {
		file.close();
		m_saveBlocked = !keepUnreadable();
		return;
	}
	QJsonParseError error {};
	const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &error);
	file.close();
	if (error.error != QJsonParseError::NoError || !document.isObject()) {
		// Keep what is there instead of letting the next save overwrite it with an empty store.
		m_saveBlocked = !keepUnreadable();
		return;
	}
	const auto root = document.object();
	for (auto it = root.constBegin(); it != root.constEnd(); ++it) {
		const auto entry = it.value().toObject();
		const QString text = entry.value("text").toString();
		if (!text.isEmpty()) {
			m_stories.insert(it.key(), {text, entry.value("model").toString(), entry.value("at").toString()});
		}
	}
}

void StoryStore::save() const
{
	// A store we could not read and could not move aside is left exactly as it is.
	if (m_saveBlocked) {
		return;
	}
	QJsonObject root;
	for (auto it = m_stories.constBegin(); it != m_stories.constEnd(); ++it) {
		root.insert(it.key(), QJsonObject {{"text", it->text}, {"model", it->model}, {"at", it->at}});
	}
	if (ensureFileParent(m_path)) {
		safeWriteFile(m_path, QJsonDocument(root).toJson(QJsonDocument::Compact));
	}
}
