#include "discover-dialogs.h"
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QPushButton>
#include <QRegularExpression>
#include <QTableWidget>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include "discovery-feed.h"
#include "models/image.h"
#include "models/site.h"
#include "thumbnail-loader.h"


namespace
{
	QString kindName(const QString &type)
	{
		if (type == QLatin1String("artist")) {
			return DiscoverTasteDialog::tr("Artist");
		}
		if (type == QLatin1String("character")) {
			return DiscoverTasteDialog::tr("Character");
		}
		if (type == QLatin1String("copyright")) {
			return DiscoverTasteDialog::tr("Series");
		}
		return DiscoverTasteDialog::tr("Tag");
	}

	QString strength(double weight)
	{
		if (weight < 0) {
			return DiscoverTasteDialog::tr("avoided (%1%)").arg(int(std::round(weight * 100)));
		}
		const int filled = std::clamp(int(std::round(weight * 8)), 1, 10);
		return QString(filled, QChar(0x25B0)) + QString(std::max(0, 8 - filled), QChar(0x25B1)) + QStringLiteral("  %1%").arg(int(std::round(weight * 100)));
	}
}


DiscoverTasteDialog::DiscoverTasteDialog(DiscoveryFeed *feed, QWidget *parent)
	: QDialog(parent), m_feed(feed)
{
	setObjectName("discoverTasteDialog");
	setWindowTitle(tr("Your Discover taste"));
	resize(820, 760);
	const auto &taste = feed->taste();
	const auto &tuning = feed->tuning();
	auto *layout = new QVBoxLayout(this);

	auto *how = new QLabel(tr("<b>How Discover picks pictures</b><br>"
		"It searches your selected sources with the tags that set your likes and favorites apart. Each tag is compared with recent posts "
		"sampled from the same sources, so themes, artists and characters you pick far more often than other people outrank tags nearly every post has. "
		"Favorites count 3× a like, recent ratings a bit more. Searches whose pictures you rate come back more often; ignored or hidden ones fade. "
		"Pictures already in your Library (also copies from other sources), blacklisted or hidden never show up."), this);
	how->setObjectName("discoverHow");
	how->setWordWrap(true);
	layout->addWidget(how);

	double sampled = 0;
	int sampledSources = 0;
	for (auto it = taste.siteWeights().constBegin(); it != taste.siteWeights().constEnd(); ++it) {
		const double posts = feed->background().posts(it.key());
		sampled += posts;
		sampledSources += posts > 0 ? 1 : 0;
	}
	auto *stats = new QLabel(tr("Learned from %n favorite(s)", "", taste.favorites()) + tr(" and %n like(s)", "", taste.likes())
		+ tr(" · %1 posts sampled on %n source(s)", "", sampledSources).arg(int(sampled)) + tr(" · %n picture(s) hidden", "", feed->dismissedCount()), this);
	stats->setObjectName("discoverStats");
	stats->setWordWrap(true);
	layout->addWidget(stats);

	auto *form = new QFormLayout;
	m_focus = new QLineEdit(tuning.focus.join(' '), this);
	m_focus->setObjectName("discoverFocus");
	m_focus->setClearButtonEnabled(true);
	m_focus->setPlaceholderText(tr("Tags to focus on right now, e.g. beach swimsuit"));
	m_focus->setToolTip(tr("Most searches use these tags until you clear them; pictures with them rank first."));
	form->addRow(tr("Today's focus"), m_focus);
	m_variety = new QComboBox(this);
	m_variety->setObjectName("discoverVariety");
	m_variety->addItem(tr("Familiar: only my strongest tastes"), TasteTuning::Familiar);
	m_variety->addItem(tr("Balanced"), TasteTuning::Balanced);
	m_variety->addItem(tr("Adventurous: try fainter tastes too"), TasteTuning::Adventurous);
	m_variety->setCurrentIndex(std::max(0, m_variety->findData(tuning.variety)));
	m_varietyHint = new QLabel(this);
	m_varietyHint->setWordWrap(true);
	form->addRow(tr("Variety"), m_variety);
	form->addRow(QString(), m_varietyHint);
	layout->addLayout(form);
	connect(m_variety, &QComboBox::currentIndexChanged, this, &DiscoverTasteDialog::updateVariety);
	updateVariety();

	layout->addWidget(new QLabel(tr("<b>What Discover learned</b> · set any tag to More, Less or Never"), this));
	m_tags = new QTableWidget(0, 6, this);
	m_tags->setObjectName("discoverTasteTags");
	m_tags->setHorizontalHeaderLabels({tr("Tag"), tr("Kind"), tr("Strength"), tr("In your pictures"), tr("Rated from its searches"), tr("You want")});
	m_tags->verticalHeader()->hide();
	m_tags->setEditTriggers(QAbstractItemView::NoEditTriggers);
	m_tags->setSelectionMode(QAbstractItemView::NoSelection);
	m_tags->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
	for (int column = 1; column < 6; ++column) {
		m_tags->horizontalHeader()->setSectionResizeMode(column, QHeaderView::ResizeToContents);
	}
	layout->addWidget(m_tags, 1);
	QSet<QString> added;
	for (const auto &tag : taste.topTags(60)) {
		addRow(tag, tuning.tags.value(tag.name, 0));
		added.insert(tag.name);
	}
	for (auto it = tuning.tags.constBegin(); it != tuning.tags.constEnd(); ++it) {
		if (!added.contains(it.key())) {
			addRow({it.key(), {}, taste.weight(it.key()), 0}, it.value());
			added.insert(it.key());
		}
	}
	for (const auto &tag : taste.avoidedTags(25)) {
		if (!added.contains(tag.name)) {
			addRow(tag, 0);
			added.insert(tag.name);
		}
	}

	auto *addLayout = new QHBoxLayout;
	m_newTag = new QLineEdit(this);
	m_newTag->setObjectName("discoverNewTag");
	m_newTag->setPlaceholderText(tr("Any tag, e.g. umbrella"));
	addLayout->addWidget(new QLabel(tr("Add a tag:"), this));
	addLayout->addWidget(m_newTag, 1);
	for (const auto &choice : QList<QPair<QString, int>> {{tr("More"), TasteTuning::More}, {tr("Less"), TasteTuning::Less}, {tr("Never"), TasteTuning::Never}}) {
		auto *button = new QPushButton(choice.first, this);
		button->setObjectName("discoverAdd" + QString::number(choice.second + 2));
		connect(button, &QPushButton::clicked, this, [this, preference = choice.second]() { addTag(preference); });
		addLayout->addWidget(button);
	}
	layout->addLayout(addLayout);

	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
	buttons->button(QDialogButtonBox::Ok)->setText(tr("Apply"));
	auto *reset = buttons->addButton(tr("Reset tuning"), QDialogButtonBox::ResetRole);
	reset->setObjectName("discoverResetTuning");
	connect(reset, &QPushButton::clicked, this, [this]() {
		m_focus->clear();
		m_variety->setCurrentIndex(m_variety->findData(TasteTuning::Balanced));
		for (int row = 0; row < m_tags->rowCount(); ++row) {
			static_cast<QComboBox*>(m_tags->cellWidget(row, 5))->setCurrentIndex(0);
		}
	});
	connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
	layout->addWidget(buttons);
}

void DiscoverTasteDialog::updateVariety()
{
	const int variety = m_variety->currentData().toInt();
	m_varietyHint->setText(variety == TasteTuning::Familiar ? tr("Searches only your strongest tastes and keeps the closest matches.")
		: variety == TasteTuning::Adventurous ? tr("Mixes in tags you liked less often and keeps more of what it finds.")
		: tr("Mostly your strongest tastes, with some room to explore."));
}

void DiscoverTasteDialog::addRow(const TasteTag &tag, int preference)
{
	const int row = m_tags->rowCount();
	m_tags->insertRow(row);
	auto *name = new QTableWidgetItem(tag.name);
	name->setData(Qt::UserRole, tag.name);
	m_tags->setItem(row, 0, name);
	m_tags->setItem(row, 1, new QTableWidgetItem(kindName(tag.type)));
	m_tags->setItem(row, 2, new QTableWidgetItem(tag.weight != 0 ? strength(tag.weight) : tr("not learned yet")));
	m_tags->setItem(row, 3, new QTableWidgetItem(tag.count > 0 ? QString::number(tag.count) : QStringLiteral("—")));
	const auto feedback = m_feed->feedback().value(tag.name);
	m_tags->setItem(row, 4, new QTableWidgetItem(feedback.shown >= 1 ? tr("%1 of %2 shown").arg(int(std::round(feedback.rated))).arg(int(std::round(feedback.shown))) : QStringLiteral("—")));
	auto *choice = new QComboBox(m_tags);
	choice->addItem(tr("Auto"), 0);
	choice->addItem(tr("More"), int(TasteTuning::More));
	choice->addItem(tr("Less"), int(TasteTuning::Less));
	choice->addItem(tr("Never"), int(TasteTuning::Never));
	choice->setCurrentIndex(std::max(0, choice->findData(preference)));
	choice->setAccessibleName(tr("Preference for %1").arg(tag.name));
	m_tags->setCellWidget(row, 5, choice);
}

void DiscoverTasteDialog::addTag(int preference)
{
	const QString name = TasteProfile::normalize(m_newTag->text().trimmed().replace(' ', '_'));
	if (name.isEmpty()) {
		return;
	}
	for (int row = 0; row < m_tags->rowCount(); ++row) {
		if (m_tags->item(row, 0)->data(Qt::UserRole).toString() == name) {
			auto *choice = static_cast<QComboBox*>(m_tags->cellWidget(row, 5));
			choice->setCurrentIndex(choice->findData(preference));
			m_tags->scrollToItem(m_tags->item(row, 0));
			m_newTag->clear();
			return;
		}
	}
	addRow({name, {}, m_feed->taste().weight(name), 0}, preference);
	m_tags->scrollToBottom();
	m_newTag->clear();
}

TasteTuning DiscoverTasteDialog::tuning() const
{
	TasteTuning result;
	for (const QString &part : m_focus->text().split(QRegularExpression("[\\s,]+"), Qt::SkipEmptyParts)) {
		const QString name = TasteProfile::normalize(part);
		if (!name.isEmpty() && !result.focus.contains(name)) {
			result.focus.append(name);
		}
	}
	result.variety = m_variety->currentData().toInt();
	for (int row = 0; row < m_tags->rowCount(); ++row) {
		const int preference = static_cast<QComboBox*>(m_tags->cellWidget(row, 5))->currentData().toInt();
		if (preference != 0) {
			result.tags.insert(m_tags->item(row, 0)->data(Qt::UserRole).toString(), preference);
		}
	}
	return result;
}


DiscoverHiddenDialog::DiscoverHiddenDialog(DiscoveryFeed *feed, QWidget *parent)
	: QDialog(parent), m_feed(feed)
{
	setObjectName("discoverHiddenDialog");
	setWindowTitle(tr("Hidden pictures"));
	resize(860, 640);
	auto *layout = new QVBoxLayout(this);
	auto *intro = new QLabel(tr("Pictures you marked Not interested, newest first. Restore any you hid by mistake: they can show up again and "
		"Discover takes back what hiding them taught it. Tags you chose to see less of are listed in Tune taste."), this);
	intro->setWordWrap(true);
	layout->addWidget(intro);

	m_list = new QListWidget(this);
	m_list->setObjectName("discoverHiddenList");
	m_list->setViewMode(QListView::IconMode);
	m_list->setIconSize(QSize(150, 150));
	m_list->setGridSize(QSize(176, 206));
	m_list->setResizeMode(QListView::Adjust);
	m_list->setMovement(QListView::Static);
	m_list->setUniformItemSizes(true);
	m_list->setWordWrap(true);
	m_list->setSelectionMode(QAbstractItemView::ExtendedSelection);
	layout->addWidget(m_list, 1);
	QPixmap placeholder(150, 150);
	placeholder.fill(QColor(127, 127, 127, 50));
	for (const auto &hidden : feed->hiddenPictures()) {
		const QString site = hidden.image->parentSite() != nullptr ? hidden.image->parentSite()->name() : QString();
		auto *item = new QListWidgetItem(QIcon(placeholder), site + "\n" + QLocale().toString(hidden.hiddenAt.toLocalTime(), QLocale::ShortFormat), m_list);
		item->setData(Qt::UserRole, hidden.key);
		item->setToolTip(hidden.image->tagsString().mid(0, 30).join(' '));
		m_images.insert(hidden.key, hidden.image);
		m_pending.append(hidden.key);
	}

	m_summary = new QLabel(this);
	m_summary->setWordWrap(true);
	layout->addWidget(m_summary);
	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
	m_restoreSelected = buttons->addButton(tr("Restore selected"), QDialogButtonBox::ActionRole);
	m_restoreSelected->setObjectName("discoverRestoreSelected");
	m_restoreSelected->setEnabled(false);
	m_restoreAll = buttons->addButton(tr("Restore all"), QDialogButtonBox::ActionRole);
	m_restoreAll->setObjectName("discoverRestoreAll");
	layout->addWidget(buttons);
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
	connect(m_list, &QListWidget::itemSelectionChanged, this, [this]() { m_restoreSelected->setEnabled(!m_list->selectedItems().isEmpty()); });
	connect(m_restoreSelected, &QPushButton::clicked, this, [this]() {
		QStringList keys;
		for (auto *item : m_list->selectedItems()) {
			keys.append(item->data(Qt::UserRole).toString());
		}
		restoreKeys(keys);
	});
	connect(m_restoreAll, &QPushButton::clicked, this, [this]() {
		const int total = m_feed->dismissedCount();
		m_feed->resetDismissed();
		m_restored += total;
		m_list->clear();
		m_pending.clear();
		updateSummary();
	});
	updateSummary();
	loadNextThumbnails();
}

DiscoverHiddenDialog::~DiscoverHiddenDialog()
{
	for (const auto &loader : std::as_const(m_loaders)) {
		if (loader) {
			disconnect(loader, nullptr, this, nullptr);
			loader->abort();
		}
	}
}

int DiscoverHiddenDialog::restoredCount() const
{
	return m_restored;
}

void DiscoverHiddenDialog::restoreKeys(const QStringList &keys)
{
	m_feed->restore(keys);
	m_restored += int(keys.size());
	for (int row = m_list->count() - 1; row >= 0; --row) {
		if (keys.contains(m_list->item(row)->data(Qt::UserRole).toString())) {
			delete m_list->takeItem(row);
		}
	}
	for (const QString &key : keys) {
		m_pending.removeAll(key);
	}
	updateSummary();
}

void DiscoverHiddenDialog::updateSummary()
{
	const int older = m_feed->dismissedCount() - m_list->count();
	QString text = tr("%n hidden picture(s).", "", m_feed->dismissedCount());
	if (older > 0) {
		text += ' ' + tr("%n were hidden before previews were kept; Restore all brings them back too.", "", older);
	}
	m_summary->setText(text);
	m_restoreAll->setEnabled(m_feed->dismissedCount() > 0);
}

void DiscoverHiddenDialog::loadNextThumbnails()
{
	// A few at a time: hidden pictures may all come from one rate-limited source.
	while (m_loaders.size() < 4 && !m_pending.isEmpty()) {
		const QString key = m_pending.takeFirst();
		auto *loader = new ThumbnailLoader(m_images.value(key), QSize(150, 150), true, this);
		m_loaders.append(loader);
		connect(loader, &ThumbnailLoader::finished, this, [this, loader, key]() {
			m_loaders.removeAll(loader);
			loader->deleteLater();
			const QPixmap preview = m_images.value(key)->previewImage();
			for (int row = 0; row < m_list->count() && !preview.isNull(); ++row) {
				if (m_list->item(row)->data(Qt::UserRole).toString() == key) {
					m_list->item(row)->setIcon(QIcon(preview.scaled(150, 150, Qt::KeepAspectRatio, Qt::SmoothTransformation)));
				}
			}
			loadNextThumbnails();
		}, Qt::QueuedConnection);
		loader->load();
	}
}
