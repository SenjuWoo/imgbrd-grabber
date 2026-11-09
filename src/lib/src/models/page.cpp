#include "models/page.h"
#include <QDateTime>
#include <QUrl>
#include <algorithm>
#include <utility>
#include "analytics.h"
#include "functions.h"
#include "logger.h"
#include "models/api/api.h"
#include "models/search-query/search-query.h"
#include "models/site.h"


Page::Page(Profile *profile, Site *site, const QList<Site*> &sites, SearchQuery query, int page, int limit, const QStringList &postFiltering, bool smart, QObject *parent, int pool, const PageInformation &lastPageInformation)
	: QObject(parent), m_site(site), m_regexApi(-1), m_query(std::move(query)), m_errors(QStringList()), m_imagesPerPage(limit), m_smart(smart)
{
	m_website = m_site->url();
	m_imagesCount = -1;
	m_pagesCount = -1;

	// Add site-level automatically added tags
	if (m_query.gallery == nullptr) {
		for (const QString &addedTag : m_site->setting("added_tags").toString().split(" ", Qt::SkipEmptyParts)) {
			if (!m_query.tags.contains(addedTag)) {
				m_query.tags.append(addedTag);
			}
		}
		for (const QString &removedTag : m_site->setting("removed_tags").toString().split(" ", Qt::SkipEmptyParts)) {
			m_query.tags.removeAll(removedTag);
		}
	}

	if (!m_query.tags.isEmpty()) {
		// Replace shortcuts to increase compatibility
		QString text = " " + m_query.tags.join(' ') + " ";
		text.replace(" rating:s ", " rating:safe ", Qt::CaseInsensitive)
			.replace(" rating:g ", " rating:general ", Qt::CaseInsensitive)
			.replace(" rating:q ", " rating:questionable ", Qt::CaseInsensitive)
			.replace(" rating:e ", " rating:explicit ", Qt::CaseInsensitive)
			.replace(" -rating:s ", " -rating:safe ", Qt::CaseInsensitive)
			.replace(" -rating:g ", " -rating:general ", Qt::CaseInsensitive)
			.replace(" -rating:q ", " -rating:questionable ", Qt::CaseInsensitive)
			.replace(" -rating:e ", " -rating:explicit ", Qt::CaseInsensitive);
		QStringList tags = text.split(" ", Qt::SkipEmptyParts);

		// Get the list of all enabled modifiers
		QStringList modifiers = QStringList();
		for (Site *ste : sites) {
			if (!ste->getApis().isEmpty()) {
				modifiers.append(ste->getApis().first()->modifiers());
			}
		}
		const QStringList mods = m_site->getApis().isEmpty() ? QStringList() : m_site->getApis().first()->modifiers();
		for (const QString &mod : mods) {
			modifiers.removeAll(mod);
		}

		// Remove modifiers from tags
		for (const QString &mod : modifiers) {
			if (tags.removeAll(mod) > 0) {
				log(QStringLiteral("[%1] Unsupported modifier removed from search: %2").arg(m_site->url(), mod), Logger::Warning);
			}
		}

		m_search = tags;
		m_query.tags = m_search;
	}

	// Generate pages
	PostFilter postFilter(postFiltering);
	const QList<Api*> availableApis = m_site->getLoggedInApis();
	m_pageApis.reserve(availableApis.count());
	for (Api *api : availableApis) {
		// Only use APIs supporting the requested endpoint
		if (!m_query.endpoint.isEmpty() && !api->endpoints().contains(m_query.endpoint)) {
			continue;
		}

		auto *pageApi = new PageApi(this, profile, m_site, api, m_query, page, limit, postFilter, smart, this, pool, lastPageInformation);
		if (m_pageApis.count() == 0) {
			connect(pageApi, &PageApi::httpsRedirect, this, &Page::httpsRedirectSlot);
		}
		m_siteApis.append(api);
		m_pageApis.append(pageApi);
		if (api->getName() == QLatin1String("Html") && m_regexApi < 0) {
			m_regexApi = m_pageApis.count() - 1;
		}
	}
	m_currentApi = -1;

	// Set values
	m_page = page;
	m_pool = pool;
	fallback(false);
}
Page::~Page()
{
	qDeleteAll(m_pageApis);
}

void Page::fallback(bool loadIfPossible)
{
	if (m_currentApi >= m_pageApis.count() - 1) {
		// Pictures, or a valid but empty page: both are results. Only a page nobody could read is a failure.
		switch (m_outcome)
		{
			case Outcome::Images:
			case Outcome::Empty:
				log(QStringLiteral("[%1] The site answered with no pictures").arg(m_site->url()), Logger::Info);
				emit finishedLoading(this);
				return;
			case Outcome::Pending:
			case Outcome::Inconclusive:
			case Outcome::Failed:
			default:
				break;
		}
		log(QStringLiteral("[%1] No valid source of the site returned result.").arg(m_site->url()), Logger::Warning);
		m_errors.append(tr("No valid source of the site returned result."));
		emit failedLoading(this);
		return;
	}

	// A new attempt keeps the diagnostics gathered so far: an inconclusive page must still explain itself.
	m_currentApi++;
	if (m_currentApi > 0) {
		log(QStringLiteral("[%1] Loading using %2 failed. Retry using %3.").arg(m_site->url(), m_siteApis[m_currentApi - 1]->getName(), m_siteApis[m_currentApi]->getName()), Logger::Warning);
	}

	if (loadIfPossible) {
		loadApi();
	}
}

PageInformation Page::pageInformation() const
{
	return currentApi() ? currentApi()->pageInformation() : PageInformation{};
}

void Page::setLastPage(const PageInformation& info)
{
	for (PageApi *api : qAsConst(m_pageApis)) {
		api->setLastPage(info);
	}

	m_currentApi--;
	fallback(false);
}

void Page::load(bool rateLimit)
{
	// A caller starting a load gets a fresh budget and a fresh outcome; an API fallback keeps both.
	m_rateLimitDeadline = 0;
	m_outcome = Outcome::Pending;
	m_errors.clear();
	loadApi(rateLimit);
}
void Page::loadApi(bool rateLimit)
{
	if (m_currentApi < 0 || m_currentApi >= m_pageApis.count()) {
		log(QStringLiteral("[%1] No available API to perform the request").arg(m_site->url()), Logger::Error);
		m_errors.append(tr("No available API to perform the request."));
		emit failedLoading(this);
		return;
	}

	connect(m_pageApis[m_currentApi], &PageApi::finishedLoading, this, &Page::loadFinished, Qt::UniqueConnection);
	m_pageApis[m_currentApi]->load(rateLimit);
}
void Page::loadFinished(PageApi *api, PageApi::LoadResult status)
{
	if (api != currentApi()) {
		return;
	}

	QString eventLabel = QStringLiteral("%1 (%2)").arg(m_site->url(), m_siteApis[m_currentApi]->getName());
	const bool lastApi = m_currentApi >= m_pageApis.count() - 1;

	// One signal per finished page: images, a confirmed empty result, or a failure with its reasons.
	if (status == PageApi::LoadResult::Ok && !api->images().isEmpty()) {
		m_outcome = Outcome::Images;
		Analytics::getInstance().sendEvent("Page load", "Success", eventLabel);
		emit finishedLoading(this);
		return;
	}

	if (status == PageApi::LoadResult::Ok) {
		// A valid page with nothing in it. Only the first page is worth retrying through another API:
		// a site's API can silently drop a condition the next one understands, while later pages are
		// empty simply because the results end there.
		if (m_outcome == Outcome::Pending) {
			m_outcome = Outcome::Empty;
		}
		if (!lastApi && api->page() <= 1 && !m_query.tags.isEmpty()) {
			log(QStringLiteral("[%1] %2 returned no pictures. Retry using %3.").arg(m_site->url(), m_siteApis[m_currentApi]->getName(), m_siteApis[m_currentApi + 1]->getName()), Logger::Warning);
			fallback();
			return;
		}
		if (m_outcome == Outcome::Empty) {
			Analytics::getInstance().sendEvent("Page load", "Success", eventLabel);
			emit finishedLoading(this);
			return;
		}
		// Something earlier already failed, so this empty page cannot confirm an empty search.
		emit failedLoading(this);
		return;
	}

	// A failed request only turns the page into a failure when nothing readable came back first, and an
	// empty answer followed by a failure cannot be confirmed as a real empty search.
	if (m_outcome == Outcome::Pending) {
		m_outcome = Outcome::Failed;
	} else if (m_outcome == Outcome::Empty) {
		m_outcome = Outcome::Inconclusive;
	}
	const QStringList &errors = api->errors();
	if (errors.isEmpty() || !errors.first().contains("impossible")) {
		Analytics::getInstance().sendEvent("Page load", "Error", eventLabel);
	}
	m_errors.append(errors);
	if (api->isTerminal() || lastApi) {
		emit failedLoading(this);
		return;
	}
	fallback();
}
void Page::abort()
{
	if (m_currentApi < 0 || m_currentApi >= m_pageApis.count()) {
		return;
	}
	m_pageApis[m_currentApi]->abort();
}

void Page::loadTags()
{
	if (m_regexApi < 0) {
		return;
	}

	connect(m_pageApis[m_regexApi], &PageApi::finishedLoading, this, &Page::loadTagsFinished, Qt::UniqueConnection);
	m_pageApis[m_regexApi]->load();
}
void Page::loadTagsFinished(PageApi *api, PageApi::LoadResult status)
{
	Q_UNUSED(status)

	if (m_regexApi < 0 || api != m_pageApis[m_regexApi]) {
		return;
	}

	emit finishedLoadingTags(this);
}
void Page::abortTags()
{
	if (m_regexApi < 0) {
		return;
	}

	m_pageApis[m_regexApi]->abort();
}

void Page::httpsRedirectSlot()
{ emit httpsRedirect(this); }


void Page::clear()
{
	m_outcome = Outcome::Pending;
	m_rateLimitDeadline = 0;
	for (PageApi *pageApi : qAsConst(m_pageApis)) {
		pageApi->clear();
	}
}

qint64 Page::rateLimitBudget()
{
	const qint64 now = QDateTime::currentMSecsSinceEpoch();
	// A fresh load starts a fresh budget; later limits of the same load only draw it down.
	if (m_rateLimitDeadline == 0) {
		m_rateLimitDeadline = now + RateLimitBudget;
	}
	return qMax<qint64>(0, m_rateLimitDeadline - now);
}

PageApi *Page::currentApi() const
{
	return m_pageApis.value(m_currentApi, nullptr);
}

Site *Page::site() const { return m_site; }
const QString &Page::website() const { return m_website; }
const QString &Page::wiki() const
{
	static const QString empty;
	const auto *api = m_regexApi < 0 ? currentApi() : m_pageApis[m_regexApi];
	return api ? api->wiki() : empty;
}
const SearchQuery &Page::query() const { return m_query; }
const QStringList &Page::search() const { return m_search; }
const QStringList &Page::errors() const { return m_errors; }
int Page::imagesPerPage() const { return currentApi() ? currentApi()->imagesPerPage() : qMax(1, m_imagesPerPage); }
int Page::page() const { return m_page; }
int Page::pageImageCount() const { return currentApi() ? currentApi()->pageImageCount() : 0; }
int Page::filteredImageCount() const { return currentApi() ? currentApi()->filteredImageCount() : 0; }
const QList<QSharedPointer<Image>> &Page::images() const
{
	static const QList<QSharedPointer<Image>> empty;
	return currentApi() ? currentApi()->images() : empty;
}
const QUrl &Page::url() const
{
	static const QUrl empty;
	return currentApi() ? currentApi()->url() : empty;
}
const QUrl &Page::friendlyUrl() const
{
	static const QUrl empty;
	const auto *api = m_regexApi < 0 ? currentApi() : m_pageApis[m_regexApi];
	return api ? api->url() : empty;
}
const QList<Tag> &Page::tags() const
{
	static const QList<Tag> empty;
	const auto *api = m_regexApi < 0 || m_pageApis[m_regexApi]->tags().isEmpty() ? currentApi() : m_pageApis[m_regexApi];
	return api ? api->tags() : empty;
}
int Page::highLimit() const { return currentApi() ? currentApi()->highLimit() : 0; }
bool Page::hasNext() const { return currentApi() && currentApi()->hasNext(); }
bool Page::isLoaded() const { return currentApi() && currentApi()->isLoaded(); }
bool Page::isValid() const { return !m_pageApis.isEmpty(); }

QMap<QString, QUrl> Page::urls() const
{
	QMap<QString, QUrl> ret;
	for (int i = 0; i < m_pageApis.count(); ++i) {
		auto *siteApi = m_pageApis[i]->api();
		auto *pageApi = m_pageApis[i];
		if (!pageApi->url().isEmpty()) {
			ret[siteApi->getName()] = pageApi->url();
		}
	}
	return ret;
}

bool Page::hasSource() const
{
	return std::any_of(
		m_pageApis.constBegin(),
		m_pageApis.constEnd(),
		[](PageApi *pageApi) {
		return !pageApi->source().isEmpty();
	}
	);
}

int Page::imagesCount(bool guess) const
{
	if (!currentApi()) {
		return -1;
	}
	if (m_regexApi >= 0 && !m_pageApis[m_currentApi]->isImageCountSure()) {
		const int count = m_pageApis[m_regexApi]->imagesCount(guess);
		if (count >= 0) {
			return count;
		}
	}
	return m_pageApis[m_currentApi]->imagesCount(guess);
}
int Page::maxImagesCount() const
{
	if (!currentApi()) {
		return -1;
	}
	if (m_regexApi >= 0 && !m_pageApis[m_currentApi]->isImageCountSure()) {
		const int count = m_pageApis[m_regexApi]->maxImagesCount();
		if (count >= 0) {
			return count;
		}
	}
	return m_pageApis[m_currentApi]->maxImagesCount();
}
int Page::pagesCount(bool guess) const
{
	if (!currentApi()) {
		return -1;
	}
	if (m_regexApi >= 0 && !m_pageApis[m_currentApi]->isPageCountSure()) {
		const int count = m_pageApis[m_regexApi]->pagesCount(guess);
		if (count >= 0) {
			return count;
		}
	}
	return m_pageApis[m_currentApi]->pagesCount(guess);
}
int Page::maxPagesCount() const
{
	if (!currentApi()) {
		return -1;
	}
	if (m_regexApi >= 0 && !m_pageApis[m_currentApi]->isPageCountSure()) {
		const int count = m_pageApis[m_regexApi]->maxPagesCount();
		if (count >= 0) {
			return count;
		}
	}
	return m_pageApis[m_currentApi]->maxPagesCount();
}
