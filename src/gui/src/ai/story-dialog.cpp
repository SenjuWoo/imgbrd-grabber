#include "ai/story-dialog.h"
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProgressDialog>
#include <QPushButton>
#include <QSet>
#include <QSettings>
#include <QVBoxLayout>
#include "ai/picture-story.h"
#include "ai/story-models.h"
#include "models/image.h"
#include "models/library-store.h"
#include "models/profile.h"
#include "models/story-store.h"
#include "ui/toast.h"


namespace
{
	/** The local file behind a Library picture, for imports that have no public URL to send. */
	QString localPathFor(Profile *profile, const QString &key, qint64 collection = 0)
	{
		for (const QString &path : profile->library()->entry(key, collection).localPaths) {
			if (QFileInfo::exists(path)) {
				return path;
			}
		}
		return {};
	}
}


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

	auto *modelRow = new QWidget(m_settings);
	auto *modelLayout = new QHBoxLayout(modelRow);
	modelLayout->setContentsMargins(0, 0, 0, 0);
	m_model = new QLineEdit(PictureStory::model(profile), modelRow);
	m_model->setPlaceholderText(QStringLiteral("openrouter/auto"));
	auto *find = new QPushButton(tr("Find vision models…"), modelRow);
	modelLayout->addWidget(m_model, 1);
	modelLayout->addWidget(find);

	m_auto = new QCheckBox(m_settings);
	m_auto->setChecked(PictureStory::autoModel(profile));
	m_quality = new QCheckBox(tr("Quality: read the scene first, then write from it"), m_settings);
	m_quality->setChecked(PictureStory::quality(profile));
	m_writerModel = new QLineEdit(PictureStory::writerModel(profile), m_settings);
	m_writerModel->setPlaceholderText(tr("same model as the picture"));

	form->addRow(tr("Base URL"), m_baseUrl);
	form->addRow(tr("API key"), m_apiKey);
	form->addRow(tr("Model"), modelRow);
	form->addRow(QString(), m_auto);
	form->addRow(QString(), m_quality);
	form->addRow(tr("Writing model"), m_writerModel);

	// An automatic choice makes the model field moot; a typed model is a decision and turns it off. The label
	// always says which of the two is active, and which model automatic mode settled on.
	const auto syncModelField = [this]() {
		const bool automatic = m_auto->isChecked();
		m_model->setEnabled(!automatic);
		m_writerModel->setEnabled(m_quality->isChecked());
		if (!automatic) {
			m_auto->setText(tr("Manual model"));
			return;
		}
		const QString cached = PictureStory::cachedAutoModel(m_profile);
		m_auto->setText(cached.isEmpty()
			? tr("Automatic: best free vision model (looked up on the first story)")
			: tr("Automatic: best free vision model (currently %1)").arg(cached));
	};
	syncModelField();
	connect(m_auto, &QCheckBox::toggled, this, syncModelField);
	connect(m_quality, &QCheckBox::toggled, this, syncModelField);
	connect(m_model, &QLineEdit::textEdited, this, [this](const QString &text) {
		if (!text.trimmed().isEmpty() && m_auto->isChecked()) {
			m_auto->setChecked(false);
		}
	});
	connect(find, &QPushButton::clicked, this, &StoryDialog::pickVisionModel);

	auto *buttons = new QDialogButtonBox(this);
	m_generate = buttons->addButton(tr("Write a story"), QDialogButtonBox::ActionRole);
	// Only offered after the picture could not be used: writing from tags is a choice, never a silent fallback.
	m_tagsOnly = buttons->addButton(tr("Write from tags only"), QDialogButtonBox::ActionRole);
	m_tagsOnly->setVisible(false);
	auto *copy = buttons->addButton(tr("Copy"), QDialogButtonBox::ActionRole);
	auto *settings = buttons->addButton(tr("AI settings"), QDialogButtonBox::ActionRole);
	buttons->addButton(QDialogButtonBox::Close);

	auto *layout = new QVBoxLayout(this);
	layout->addWidget(m_text, 1);
	layout->addWidget(m_status);
	layout->addWidget(m_settings);
	layout->addWidget(buttons);

	connect(m_generate, &QPushButton::clicked, this, [this]() { generate(); });
	connect(m_tagsOnly, &QPushButton::clicked, this, [this]() { generate(true); });
	connect(copy, &QPushButton::clicked, this, [this]() {
		QApplication::clipboard()->setText(m_text->toPlainText());
	});
	connect(settings, &QPushButton::clicked, this, [this]() { showSettings(m_settings->isHidden()); });
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
	connect(m_story, &PictureStory::ready, this, [this](const QString &key, const QString &text) {
		setStory(text, m_story->modelUsed());
		if (m_story->usedPicture()) {
			m_status->setText(PictureStory::quality(m_profile)
				? tr("Written from the scene read off the picture by %1.").arg(m_story->modelUsed())
				: tr("Written from the picture and its tags by %1.").arg(m_story->modelUsed()));
		} else {
			m_status->setText(tr("Written from tags only by %1, because no picture was sent.").arg(m_story->modelUsed()));
		}
		m_generate->setEnabled(true);
		m_tagsOnly->setVisible(false);
		Q_UNUSED(key)
	});
	connect(m_story, &PictureStory::failed, this, [this](const QString &key, const QString &error) {
		m_status->setText(error);
		m_generate->setEnabled(true);
		// Offer the three ways forward the failure allows, and only those.
		const PictureStory::Error kind = m_story->errorKind();
		const bool pictureProblem = kind == PictureStory::Error::ImageRejected || kind == PictureStory::Error::NoPicture;
		m_tagsOnly->setVisible(pictureProblem && !m_image.isNull());
		if (kind == PictureStory::Error::InvalidModel || kind == PictureStory::Error::ImageRejected) {
			showSettings(true);
		}
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
		m_status->setText(PictureStory::settingsProblem(profile));
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
	PictureStory::setAutoModel(m_profile, m_auto->isChecked());
	PictureStory::setQuality(m_profile, m_quality->isChecked());
	PictureStory::setWriterModel(m_profile, m_writerModel->text());
	m_profile->getSettings()->sync();
}

void StoryDialog::setStory(const QString &text, const QString &model)
{
	m_text->setPlainText(text);
	StoryStore(m_profile->getPath()).setStory(m_key, text, model);
}

void StoryDialog::pickVisionModel()
{
	// The provider's own list decides here too; nothing paid is ever chosen without the user seeing it.
	saveSettings();
	auto *catalog = new StoryModelCatalog(m_profile, this);
	// Parent-owned and only closed, never destroyed while replies are still in flight.
	auto *progress = new QProgressDialog(tr("Reading the model list…"), tr("Cancel"), 0, 0, this);
	progress->setWindowModality(Qt::WindowModal);
	progress->setMinimumDuration(0);
	connect(catalog, &StoryModelCatalog::failed, this, [this, progress](const QString &error) {
		progress->close();
		m_status->setText(error);
	});
	connect(catalog, &StoryModelCatalog::ready, this, [this, progress](const QList<StoryModel> &models) {
		progress->close();
		QStringList labels;
		QStringList ids;
		for (const StoryModel &model : models) {
			if (!model.images) {
				continue;
			}
			const QString price = model.free ? tr("free") : tr("paid");
			labels.append(QStringLiteral("%1 — %2, %3").arg(model.id, price, tr("reads images")));
			ids.append(model.id);
		}
		if (ids.isEmpty()) {
			m_status->setText(tr("This provider lists no model that reads images; type the model id yourself."));
			return;
		}
		bool accepted = false;
		const QString chosen = QInputDialog::getItem(this, tr("Vision models"), tr("Model:"), labels, 0, false, &accepted);
		if (!accepted) {
			return;
		}
		const int index = labels.indexOf(chosen);
		if (index >= 0) {
			m_model->setText(ids.at(index));
			m_auto->setChecked(false);
		}
	});
	catalog->fetch();
	progress->exec();
}

void StoryDialog::generate(bool tagsOnly)
{
	if (m_image.isNull()) {
		return;
	}
	saveSettings();
	if (!PictureStory::configured(m_profile)) {
		showSettings(true);
		m_status->setText(PictureStory::settingsProblem(m_profile));
		return;
	}
	showSettings(false);
	m_generate->setEnabled(false);
	m_tagsOnly->setVisible(false);
	m_status->setText(tagsOnly ? tr("Writing from tags…") : tr("Writing…"));
	m_story->generate(m_image, localPathFor(m_profile, m_key), tagsOnly);
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
				// Favorites can be set on the whole Library and inside a collection: take both, once each.
				StoryStore store(profile->getPath());
				QSet<QString> collected;
				const auto add = [this, &store, &collected](qint64 collection) {
					for (const auto &entry : m_profile->library()->ratedEntries(collection)) {
						if (!entry.favorite || entry.image.isEmpty() || store.contains(entry.key) || collected.contains(entry.key)) {
							continue;
						}
						auto image = QSharedPointer<Image>::create(m_profile);
						if (image->read(entry.image, m_profile->getSites())) {
							collected.insert(entry.key);
							m_keys.append(entry.key);
							m_paths.append(localPathFor(m_profile, entry.key, collection));
							m_images.append(image);
						}
					}
				};
				add(0);
				for (const auto &collection : m_profile->library()->collections()) {
					add(collection.id);
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
					m_store.setStory(m_keys.value(m_index), text, m_writer->modelUsed());
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
				m_writer->generate(m_images.at(m_index), m_paths.value(m_index));
			}

			Profile *m_profile;
			QWidget *m_parent;
			StoryStore m_store;
			PictureStory *m_writer;
			QProgressDialog *m_progress = nullptr;
			QStringList m_keys;
			QStringList m_paths;
			QList<QSharedPointer<Image>> m_images;
			int m_index = 0;
			bool m_stopped = false;
	};
}

void StoryDialog::writeForFavorites(Profile *profile, QWidget *parent)
{
	if (!PictureStory::configured(profile)) {
		QMessageBox::information(parent, tr("Story"), PictureStory::settingsProblem(profile));
		return;
	}
	auto *batch = new StoryBatch(profile, parent);
	batch->run();
}
