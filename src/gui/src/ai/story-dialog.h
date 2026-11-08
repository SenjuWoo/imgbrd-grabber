#ifndef STORY_DIALOG_H
#define STORY_DIALOG_H

#include <QDialog>
#include <QSharedPointer>


class Image;
class Profile;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QWidget;
class PictureStory;

/**
 * Shows the AI-written story behind one picture, and writes it when there is none yet.
 * The provider settings live in the dialog the first time so no extra settings page is needed.
 */
class StoryDialog : public QDialog
{
	Q_OBJECT

	public:
		StoryDialog(Profile *profile, const QSharedPointer<Image> &image, QWidget *parent = nullptr);
		~StoryDialog() override;

		/** Writes a story for every favorited Library picture that has none, one at a time. */
		static void writeForFavorites(Profile *profile, QWidget *parent);

	private:
		void generate();
		void showSettings(bool visible);
		void saveSettings();
		void setStory(const QString &text, const QString &model);

		Profile *m_profile;
		QSharedPointer<Image> m_image;
		QString m_key;
		PictureStory *m_story;
		QPlainTextEdit *m_text;
		QLabel *m_status;
		QWidget *m_settings;
		QLineEdit *m_baseUrl;
		QLineEdit *m_apiKey;
		QLineEdit *m_model;
		QPushButton *m_generate;
};

#endif // STORY_DIALOG_H
