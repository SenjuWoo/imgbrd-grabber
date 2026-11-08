#include "models/story-store.h"
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include "utils/file-utils.h"


namespace
{
	// Stories are a few hundred characters each; the cap only stops the file from growing without bound.
	constexpr int MaxStories = 5000;
	constexpr qint64 MaxFileSize = 32 * 1024 * 1024;
}

StoryStore::StoryStore(const QString &profileDirectory)
	: m_path(QDir(profileDirectory).filePath("stories.json"))
{
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
	if (!m_stories.contains(key) && m_stories.size() >= MaxStories) {
		// Forget the oldest story rather than the newest one the user asked for.
		QString oldest;
		for (auto it = m_stories.constBegin(); it != m_stories.constEnd(); ++it) {
			if (oldest.isEmpty() || it->at < m_stories.value(oldest).at) {
				oldest = it.key();
			}
		}
		m_stories.remove(oldest);
	}
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
	if (!file.open(QIODevice::ReadOnly) || file.size() > MaxFileSize) {
		return;
	}
	const auto root = QJsonDocument::fromJson(file.readAll()).object();
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
	QJsonObject root;
	for (auto it = m_stories.constBegin(); it != m_stories.constEnd(); ++it) {
		root.insert(it.key(), QJsonObject {{"text", it->text}, {"model", it->model}, {"at", it->at}});
	}
	if (ensureFileParent(m_path)) {
		safeWriteFile(m_path, QJsonDocument(root).toJson(QJsonDocument::Compact));
	}
}
