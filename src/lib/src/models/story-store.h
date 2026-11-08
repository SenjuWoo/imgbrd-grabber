#ifndef STORY_STORE_H
#define STORY_STORE_H

#include <QHash>
#include <QString>
#include <QStringList>


/**
 * AI-written stories for Library pictures, kept in the profile ("stories.json").
 * A story is text only: keyed by the Library picture's own key, so it survives restarts and is
 * shown again when the picture is opened. Lives beside discover.json so no Library schema change
 * is needed and older versions can still open the Library.
 */
class StoryStore
{
	public:
		explicit StoryStore(const QString &profileDirectory);

		/** The story written for this picture, empty when none was. */
		QString story(const QString &key) const;
		/** Model that wrote it, empty when none was. */
		QString model(const QString &key) const;
		bool contains(const QString &key) const;
		/** Writes or replaces the story of a picture. */
		void setStory(const QString &key, const QString &text, const QString &model);
		void remove(const QString &key);
		int count() const;
		QStringList keys() const;

	private:
		void load() const;
		void save() const;

		struct Story
		{
			QString text;
			QString model;
			QString at;
		};
		QString m_path;
		mutable QHash<QString, Story> m_stories;
		mutable bool m_loaded = false;
};

#endif // STORY_STORE_H
