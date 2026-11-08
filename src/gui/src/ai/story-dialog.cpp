#include "ai/story-dialog.h"
#include <QApplication>
#include <QClipboard>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProgressDialog>
#include <QPushButton>
#include <QVBoxLayout>
#include "ai/picture-story.h"
#include "models/image.h"
#include "models/library-store.h"
#include "models/profile.h"
#include "models/story-store.h"
#include "ui/toast.h"


StoryDialog::StoryDialog(Profile *profile, const QSharedPointer<Image> &image, QWidget *parent)
	: QDialog(parent), m_profile(profile), m_image(image), m_story(new PictureStory(profile, this))
{
	setWindowTitle(tr("Story behind the picture"));
	resize(560, 480);
	m_key = image.isNull() ? QString() : LibraryStore::imageKey(*image);
	if (m_key.isEmpty() && !image.isNull()) {
		m_key = image->pageUrl().toString();
	}

	m_text = new QPlainTextEdit(this);
	m_text->setReadOnly(true);
	m_text->setPlaceholderText(tr("The story appears here."));

	m_status = new QLabel(this);
	m_status->setWordWrap(true);

	// Provider settings; hidden once a key is saved, reachable from the button below.
	m_settings = new QWidget(this);
	auto *form = new QFormLayout(m_settings);
	form->setContentsMargins(0, 0, 0, 0);
	m_baseUrl = new QLineEdit(PictureStory::baseUrl(profile), m_settings);
	m_baseUrl->setPlaceholderText(QStringLiteral("https://openrouter.ai/api/v1"));
	m_apiKey = new QLineEdit(PictureStory::apiKey(profile), m_settings);
	m_apiKey->setEchoMode(QLineEdit::Password);
	m_apiKey->setPlaceholderText(QStringLiteral("sk-or-…"));
	m_model = new QLineEdit(PictureStory::model(profile), m_settings);
	m_model->setPlaceholderText(QStringLiteral("openrouter/auto"));
	form->addRow(tr("Base URL"), m_baseUrl);
	form->addRow(tr("API key"), m_apiKey);
	form->addRow(tr("Model"), m_model);

	auto *buttons = new QDialogButtonBox(this);
	m_generate = buttons->addButton(tr("Write a story"), QDialogButtonBox::ActionRole);
	auto *copy = buttons->addButton(tr("Copy"), QDialogButtonBox::ActionRole);
	auto *settings = buttons->addButton(tr("AI settings"), QDialogButtonBox::ActionRole);
	buttons->addButton(QDialogButtonBox::Close);

	auto *layout = new QVBoxLayout(this);
	layout->addWidget(m_text, 1);
	layout->addWidget(m_status);
	layout->addWidget(m_settings);
	layout->addWidget(buttons);

	connect(m_generate, &QPushButton::clicked, this, &StoryDialog::generate);
	connect(copy, &QPushButton::clicked, this, [this]() {
		QApplication::clipboard()->setText(m_text->toPlainText());
	});
	connect(settings, &QPushButton::clicked, this, [this]() { showSettings(m_settings->isHidden()); });
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
	connect(m_story, &PictureStory::ready, this, [this](const QString &key, const QString &text) {
		setStory(text, PictureStory::model(m_profile));
		m_status->setText(tr("Written by %1.").arg(PictureStory::model(m_profile)));
		m_generate->setEnabled(true);
		Q_UNUSED(key)
	});
	connect(m_story, &PictureStory::failed, this, [this](const QString &key, const QString &error) {
		m_status->setText(error);
		m_generate->setEnabled(true);
		Q_UNUSED(key)
	});

	// A story already written is shown as is; otherwise write it if the provider is set up.
	const QString existing = StoryStore(profile->getPath()).story(m_key);
	if (!existing.isEmpty()) {
		m_text->setPlainText(existing);
		m_status->setText(tr("Saved story."));
	} else if (PictureStory::configured(profile)) {
		generate();
	} else {
		m_status->setText(tr("Add your AI base URL and API key under AI settings, then write a story."));
	}
	showSettings(!PictureStory::configured(profile));
}

StoryDialog::~StoryDialog()
{
	m_story->abort();
}

void StoryDialog::showSettings(bool visible)
{
	m_settings->setVisible(visible);
}

void StoryDialog::saveSettings()
{
	PictureStory::configure(m_profile, m_baseUrl->text(), m_apiKey->text(), m_model->text());
}

void StoryDialog::setStory(const QString &text, const QString &model)
{
	m_text->setPlainText(text);
	StoryStore(m_profile->getPath()).setStory(m_key, text, model);
}

void StoryDialog::generate()
{
	if (m_image.isNull()) {
		return;
	}
	saveSettings();
	if (!PictureStory::configured(m_profile)) {
		showSettings(true);
		m_status->setText(tr("Add your AI base URL and API key first."));
		return;
	}
	showSettings(false);
	m_generate->setEnabled(false);
	m_status->setText(tr("Writing…"));
	m_story->generate(m_image);
}


namespace
{
	/** Writes stories one picture at a time so a slow provider cannot flood the queue. */
	class StoryBatch : public QObject
	{
		public:
			StoryBatch(Profile *profile, QWidget *parent)
				: QObject(parent), m_profile(profile), m_parent(parent), m_store(profile->getPath()), m_writer(new PictureStory(profile, this))
			{
				StoryStore store(profile->getPath());
				for (const auto &entry : profile->library()->ratedEntries(0)) {
					if (!entry.favorite || entry.image.isEmpty() || store.contains(entry.key)) {
						continue;
					}
					auto image = QSharedPointer<Image>::create(profile);
					if (image->read(entry.image, profile->getSites())) {
						m_keys.append(entry.key);
						m_images.append(image);
					}
				}
			}

			void run()
			{
				if (m_images.isEmpty()) {
					Toast::show(m_parent, tr("Every favorite already has a story."), 3000);
					deleteLater();
					return;
				}
				m_progress = new QProgressDialog(tr("Writing stories for favorites…"), tr("Stop"), 0, int(m_images.size()), m_parent);
				m_progress->setWindowModality(Qt::WindowModal);
				m_progress->setMinimumDuration(0);
				connect(m_progress, &QProgressDialog::canceled, this, [this]() {
					m_writer->abort();
					m_stopped = true;
					m_progress->close();
					deleteLater();
				});
				connect(m_writer, &PictureStory::ready, this, [this](const QString &, const QString &text) {
					m_store.setStory(m_keys.value(m_index), text, PictureStory::model(m_profile));
					++m_index;
					next();
				});
				connect(m_writer, &PictureStory::failed, this, [this](const QString &, const QString &error) {
					if (m_progress != nullptr) {
						m_progress->close();
					}
					QMessageBox::warning(m_parent, tr("Story"), tr("Stopped after %n story(ies): %1", "", m_index).arg(error));
					deleteLater();
				});
				next();
			}

		private:
			void next()
			{
				if (m_stopped) {
					return;
				}
				if (m_index >= m_images.size()) {
					if (m_progress != nullptr) {
						m_progress->close();
					}
					Toast::show(m_parent, tr("Wrote %n story(ies) for favorites.", "", int(m_images.size())), 3500);
					deleteLater();
					return;
				}
				if (m_progress != nullptr) {
					m_progress->setValue(m_index);
				}
				// m_index only moves in ready(), so the story is saved under the picture it was written for.
				m_writer->generate(m_images.at(m_index));
			}

			Profile *m_profile;
			QWidget *m_parent;
			StoryStore m_store;
			PictureStory *m_writer;
			QProgressDialog *m_progress = nullptr;
			QStringList m_keys;
			QList<QSharedPointer<Image>> m_images;
			int m_index = 0;
			bool m_stopped = false;
	};
}

void StoryDialog::writeForFavorites(Profile *profile, QWidget *parent)
{
	if (!PictureStory::configured(profile)) {
		QMessageBox::information(parent, tr("Story"), tr("Add your AI base URL and API key in the story window first."));
		return;
	}
	auto *batch = new StoryBatch(profile, parent);
	batch->run();
}
