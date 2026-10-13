#include "viewer/library-batch-source-dialog.h"
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QJsonObject>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPixmap>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QTimer>
#include <QVBoxLayout>
#include "logger.h"
#include "models/image.h"
#include "models/library-store.h"
#include "models/page.h"
#include "models/profile.h"
#include "models/search-query/search-query.h"
#include "models/site-mirrors.h"
#include "models/site.h"


LibraryBatchSourceDialog::LibraryBatchSourceDialog(Profile *profile, QWidget *parent)
	: QDialog(parent), m_profile(profile), m_store(profile->library())
{
	setObjectName("libraryBatchSourceDialog");
	setWindowTitle(tr("Find sources for pictures without tags"));
	resize(720, 600);

	// Imported pictures that carry no tags and no source, with a hash a booru can search.
	const QRegularExpression md5Pattern("^[0-9a-f]{32}$");
	for (const auto &entry : m_store->entries()) {
		const auto local = entry.image.value("local_import").toObject();
		if (local.isEmpty() || !entry.tags().isEmpty() || !entry.image.value("website").toString().isEmpty()) {
			continue;
		}
		QStringList hashes;
		for (const QString &field : { QStringLiteral("source_md5"), QStringLiteral("md5") }) {
			const QString hash = local.value(field).toString().toLower();
			if (md5Pattern.match(hash).hasMatch() && !hashes.contains(hash)) {
				hashes.append(hash);
			}
		}
		if (!hashes.isEmpty()) {
			m_jobs.append({ entry.key, entry.image.value("name").toString(), hashes });
		}
	}
	const auto &known = m_profile->getSites();
	for (const QString &url : SiteMirrors::unique(m_profile->getSettings()->value("sites").toStringList())) {
		if (known.contains(url) && !known.value(url)->getApis().isEmpty()) {
			m_sites.append(url);
		}
	}

	auto *layout = new QVBoxLayout(this);
	auto *intro = new QLabel(tr("Asks each of your sources for the exact MD5 of every imported picture that has no tags. Only the hash is sent. Nothing is linked until you check a match and press Link."), this);
	intro->setWordWrap(true);
	layout->addWidget(intro);
	m_status = new QLabel(m_jobs.isEmpty() ? tr("Every imported picture already has tags or a source.")
		: tr("%n picture(s) without tags, %1 source(s) to ask.", "", int(m_jobs.size())).arg(m_sites.size()), this);
	m_status->setObjectName("libraryBatchStatus");
	m_status->setWordWrap(true);
	layout->addWidget(m_status);
	m_progress = new QProgressBar(this);
	m_progress->setRange(0, qMax(1, int(m_jobs.size())));
	m_progress->setValue(0);
	layout->addWidget(m_progress);
	m_list = new QListWidget(this);
	m_list->setObjectName("libraryBatchMatches");
	m_list->setIconSize(QSize(72, 72));
	m_list->setAccessibleName(tr("Exact matches found"));
	layout->addWidget(m_list, 1);
	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
	m_start = buttons->addButton(tr("Start lookup"), QDialogButtonBox::ActionRole);
	m_start->setObjectName("libraryBatchStart");
	m_start->setEnabled(!m_jobs.isEmpty() && !m_sites.isEmpty());
	m_link = buttons->addButton(tr("Link checked"), QDialogButtonBox::AcceptRole);
	m_link->setObjectName("libraryBatchLink");
	m_link->setEnabled(false);
	layout->addWidget(buttons);
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
	connect(m_start, &QPushButton::clicked, this, &LibraryBatchSourceDialog::toggle);
	connect(m_link, &QPushButton::clicked, this, &LibraryBatchSourceDialog::linkChecked);
	connect(m_list, &QListWidget::itemChanged, this, [this]() {
		for (int i = 0; i < m_list->count(); ++i) {
			if (m_list->item(i)->checkState() == Qt::Checked) { m_link->setEnabled(!m_running); return; }
		}
		m_link->setEnabled(false);
	});
	m_timeout = new QTimer(this);
	m_timeout->setSingleShot(true);
	connect(m_timeout, &QTimer::timeout, this, [this]() {
		if (m_running && !m_page.isNull()) {
			log(QStringLiteral("Batch source lookup on %1 timed out").arg(m_page->site()->url()), Logger::Warning);
			nextStep(); // A slow source does not hold up the others.
		}
	});
}

LibraryBatchSourceDialog::~LibraryBatchSourceDialog()
{
	stop();
}

void LibraryBatchSourceDialog::toggle()
{
	if (m_running) {
		stop();
		m_status->setText(tr("Lookup stopped. %n match(es) found so far.", "", int(m_matches.size())));
		return;
	}
	m_running = true;
	m_job = -1;
	m_start->setText(tr("Stop"));
	m_link->setEnabled(false);
	nextJob();
}

void LibraryBatchSourceDialog::stop()
{
	m_running = false;
	m_timeout->stop();
	m_steps.clear();
	if (!m_page.isNull()) {
		disconnect(m_page, nullptr, this, nullptr);
		m_page->abort();
		m_page->deleteLater();
		m_page.clear();
	}
	m_start->setText(tr("Start lookup"));
	m_link->setEnabled(m_list->count() > 0);
}

void LibraryBatchSourceDialog::nextJob()
{
	if (!m_running) { return; }
	if (++m_job >= m_jobs.size()) {
		stop();
		m_progress->setValue(m_progress->maximum());
		m_status->setText(m_matches.isEmpty() ? tr("No source has these hashes (deleted there, or from another site). Use Find source… with a reverse image search on single pictures.")
			: tr("%1 of %2 picture(s) have an exact match. Review and link the checked ones.").arg(m_matches.size()).arg(m_jobs.size()));
		return;
	}
	m_progress->setValue(m_job);
	m_steps.clear();
	for (const QString &hash : m_jobs[m_job].hashes) {
		for (const QString &site : qAsConst(m_sites)) {
			m_steps.append({ hash, site });
		}
	}
	nextStep();
}

void LibraryBatchSourceDialog::nextStep()
{
	if (!m_running) { return; }
	if (!m_page.isNull()) {
		disconnect(m_page, nullptr, this, nullptr);
		m_page->abort();
		m_page->deleteLater();
		m_page.clear();
	}
	if (m_steps.isEmpty()) {
		nextJob();
		return;
	}
	const auto step = m_steps.takeFirst();
	Site *site = m_profile->getSites().value(step.second);
	m_status->setText(tr("%1 of %2 · %3 on %4…").arg(m_job + 1).arg(m_jobs.size()).arg(QFileInfo(m_jobs[m_job].name).fileName(), site->name()));
	m_page = new Page(m_profile, site, { site }, SearchQuery(QStringList { "md5:" + step.first }), 1, 20, {}, false, this);
	const QString hash = step.first;
	connect(m_page, &Page::finishedLoading, this, [this, hash](Page *page) { showResults(page, hash); });
	connect(m_page, &Page::failedLoading, this, [this](Page *page) {
		log(QStringLiteral("Batch source lookup on %1 failed: %2").arg(page->site()->url(), page->errors().join("; ")), Logger::Warning);
		nextStep();
	});
	m_timeout->start(30000);
	m_page->load();
}

void LibraryBatchSourceDialog::showResults(Page *page, const QString &hash)
{
	for (const auto &image : page->images()) {
		if (image->md5().compare(hash, Qt::CaseInsensitive) != 0) {
			continue;
		}
		// Keep a standalone metadata image after the search Page is discarded.
		QJsonObject metadata;
		image->write(metadata);
		auto candidate = QSharedPointer<Image>::create(m_profile);
		if (!candidate->read(metadata, m_profile->getSites())) {
			continue;
		}
		const Job &job = m_jobs[m_job];
		m_matches.append({ job.key, candidate, "User confirmed exact-md5:" + hash });
		auto *item = new QListWidgetItem(QFileInfo(job.name).fileName() + "\n→ " + candidate->parentSite()->url() + " · " + tr("Picture #%1").arg(candidate->id()) + " · " + tr("%n tag(s)", "", int(candidate->tags().size())), m_list);
		item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
		item->setCheckState(Qt::Checked);
		item->setData(Qt::UserRole, int(m_matches.size()) - 1);
		item->setToolTip(candidate->pageUrl().toString());
		QPixmap pixels;
		pixels.loadFromData(m_store->entry(job.key).thumbnail);
		if (!pixels.isNull()) {
			item->setIcon(QIcon(pixels));
		}
		m_steps.clear(); // First exact match wins; the remaining sources are not asked.
		break;
	}
	nextStep();
}

void LibraryBatchSourceDialog::linkChecked()
{
	int linked = 0, failed = 0;
	for (int i = 0; i < m_list->count(); ++i) {
		auto *item = m_list->item(i);
		if (item->checkState() != Qt::Checked) { continue; }
		const auto &match = m_matches[item->data(Qt::UserRole).toInt()];
		if (m_store->linkSource(match.key, *match.image, match.evidence)) {
			++linked;
			delete m_list->takeItem(i--);
		} else {
			++failed;
		}
	}
	m_link->setEnabled(false);
	m_status->setText(tr("Linked %n picture(s).", "", linked) + (failed > 0 ? ' ' + tr("%n could not be linked: %1", "", failed).arg(m_store->lastError()) : QString()));
}
