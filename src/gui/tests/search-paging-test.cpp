#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QScopedPointer>
#include <QSettings>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTest>
#include <QUrlQuery>
#include "custom-network-access-manager.h"
#include "models/page.h"
#include "models/profile.h"
#include "models/site.h"
#include "tabs/tag-tab.h"
#include "catch.h"
#include "source-helpers.h"

namespace
{
	class PagingTagTab : public TagTab
	{
		public:
			explicit PagingTagTab(Profile *profile) : TagTab(profile, nullptr, nullptr) {}
			using SearchTab::m_endlessLoadingEnabled;
			using SearchTab::m_failedPages;
			using SearchTab::m_images;
			using SearchTab::m_pages;
			using SearchTab::m_pendingPages;
			using SearchTab::m_thumbnailsLoading;
			using SearchTab::ui_buttonLastPage;
			using SearchTab::ui_buttonNextPage;
			using SearchTab::ui_checkMergeResults;
			using SearchTab::ui_progressMergeResults;
			using SearchTab::ui_spinPage;
	};

	Profile *makePagingProfile(const QString &path)
	{
		// Keep the real source loader, Page parser and networking; this tiny source
		// lets each reply supply cursor/count evidence that Danbooru does not return.
		QScopedPointer<Profile> bootstrap(makeLibraryProfile(path));
		bootstrap.reset();
		QFile model(path + "/sites/Danbooru (2.0)/model.js");
		REQUIRE(model.open(QIODevice::WriteOnly | QIODevice::Truncate));
		REQUIRE(model.write(R"(export var source = {
			name: "Paging fixture", modifiers: [], auth: {},
			apis: {json: {maxLimit: 200, search: {
				url: function(query, opts) { return "/posts.json?page=" + query.page + "&limit=" + opts.limit; },
				parse: function(src) { return JSON.parse(src); }
			}}}
		};)") > 0);
		model.close();
		auto *profile = new Profile(path);
		profile->getSettings()->setValue("useregexfortags", false);
		profile->getSettings()->setValue("thumbnailSmartSize", false);
		profile->getSettings()->setValue("infiniteScroll", "button");
		for (auto *site : profile->getSites()) {
			site->setAutoLogin(false);
			site->setSetting("download/throttle_page", 0, 1);
			site->loadConfig();
		}
		return profile;
	}

	QString pagingReply(QTemporaryDir &directory, const QString &name, int id, QJsonObject extra = {})
	{
		QJsonArray images;
		if (id > 0) {
			images.append(QJsonObject {
				{"id", id}, {"file_url", "https://test.invalid/picture" + QString::number(id) + ".png"},
				{"md5", QString::number(id, 16).rightJustified(32, '0')}, {"tags", "forest"}
			});
		}
		extra.insert("images", images);
		QFile response(directory.filePath(name));
		REQUIRE(response.open(QIODevice::WriteOnly));
		REQUIRE(response.write(QJsonDocument(extra).toJson()) > 0);
		response.close();
		return response.fileName();
	}
}

TEST_CASE("Search Next survives a final source failure and short capped pages", "[foundation][search][paging]")
{
	REQUIRE(CustomNetworkAccessManager::NextFiles.isEmpty());
	const auto cleanup = qScopeGuard([]() { CustomNetworkAccessManager::NextFiles.clear(); });
	QTemporaryDir directory;
	REQUIRE(directory.isValid());
	const QScopedPointer<Profile> profile(makePagingProfile(directory.path()));
	auto *good = profile->getSites().value("danbooru.donmai.us");
	auto *failed = profile->getSites().value("hijiribe.donmai.us");
	REQUIRE(good != nullptr);
	REQUIRE(failed != nullptr);
	PagingTagTab tab(profile.data());
	tab.setSources({good, failed});
	tab.setImagesPerPage(400);
	tab.ui_checkMergeResults->setChecked(true);
	CustomNetworkAccessManager::NextFiles.enqueue(pagingReply(directory, "short.json", 101));
	CustomNetworkAccessManager::NextFiles.enqueue("404");
	tab.setTags("forest");
	REQUIRE(tab.m_pendingPages.size() == 2);
	QStringList completed;
	QObject::connect(tab.m_pages.value(good->url()).last().data(), &Page::finishedLoading, &tab, [&completed]() { completed.append("good"); });
	QObject::connect(tab.m_pages.value(failed->url()).last().data(), &Page::failedLoading, &tab, [&completed]() { completed.append("failed"); });
	REQUIRE(QTest::qWaitFor([&tab]() { return tab.m_pendingPages.isEmpty() && tab.m_thumbnailsLoading.isEmpty(); }, 5000));
	REQUIRE(completed == QStringList{"good", "failed"});
	REQUIRE(tab.m_failedPages.size() == 1);
	REQUIRE(tab.ui_progressMergeResults->value() == 2);
	REQUIRE(tab.ui_progressMergeResults->maximum() == 2);
	const auto first = tab.m_pages.value(good->url()).last();
	REQUIRE(first->imagesPerPage() == 200);
	REQUIRE(QUrlQuery(first->url()).queryItemValue("limit") == "200");
	REQUIRE(first->pageImageCount() == 1);
	REQUIRE(first->hasNext());
	REQUIRE(tab.ui_buttonNextPage->isEnabled());
	REQUIRE(tab.m_endlessLoadingEnabled);
	REQUIRE_FALSE(tab.ui_buttonLastPage->isEnabled());
	REQUIRE(tab.ui_spinPage->maximum() > 1);
	REQUIRE(tab.m_images.size() == 1);

	// The next page must actually load, rather than merely enabling its button.
	CustomNetworkAccessManager::NextFiles.enqueue(pagingReply(directory, "next.json", 102));
	CustomNetworkAccessManager::NextFiles.enqueue("404");
	tab.nextPage();
	REQUIRE(QTest::qWaitFor([&tab]() { return tab.m_pendingPages.isEmpty() && tab.m_thumbnailsLoading.isEmpty(); }, 5000));
	REQUIRE(tab.ui_spinPage->value() == 2);
	const auto next = tab.m_pages.value(good->url()).last();
	REQUIRE(next->page() == 2);
	REQUIRE(next->pageImageCount() == 1);
	REQUIRE_FALSE(tab.m_failedPages.contains(next.data()));
	REQUIRE(tab.m_images.size() == 1);
	REQUIRE(tab.m_images.first()->id() == 102);
	REQUIRE(tab.ui_buttonNextPage->isEnabled());
	REQUIRE(CustomNetworkAccessManager::NextFiles.isEmpty());
}

TEST_CASE("Search carries each source cursor beyond stale source totals", "[foundation][search][paging]")
{
	REQUIRE(CustomNetworkAccessManager::NextFiles.isEmpty());
	const auto cleanup = qScopeGuard([]() { CustomNetworkAccessManager::NextFiles.clear(); });
	QTemporaryDir directory;
	REQUIRE(directory.isValid());
	const QScopedPointer<Profile> profile(makePagingProfile(directory.path()));
	auto *first = profile->getSites().value("danbooru.donmai.us");
	auto *second = profile->getSites().value("hijiribe.donmai.us");
	REQUIRE(first != nullptr);
	REQUIRE(second != nullptr);
	PagingTagTab tab(profile.data());
	tab.setSources({first, second});
	tab.setImagesPerPage(400);
	tab.ui_checkMergeResults->setChecked(true);
	QJsonObject count;
	SECTION("A tag estimate never clamps the page selector")
	{
		count.insert("tags", QJsonArray{QJsonObject{{"name", "forest"}, {"count", 1}}});
	}
	SECTION("An explicit cursor overrides a stale exact total")
	{
		count.insert("imageCount", 1);
		count.insert("pageCount", 1);
	}
	auto firstReply = count;
	firstReply.insert("urlNextPage", "/posts.json?after=first-source");
	auto secondReply = count;
	secondReply.insert("urlNextPage", "/posts.json?after=second-source");
	CustomNetworkAccessManager::NextFiles.enqueue(pagingReply(directory, "first.json", 201, firstReply));
	CustomNetworkAccessManager::NextFiles.enqueue(pagingReply(directory, "second.json", 202, secondReply));
	tab.setTags("forest");
	REQUIRE(QTest::qWaitFor([&tab]() { return tab.m_pendingPages.isEmpty() && tab.m_thumbnailsLoading.isEmpty(); }, 5000));
	REQUIRE(tab.ui_buttonNextPage->isEnabled());
	REQUIRE_FALSE(tab.ui_buttonLastPage->isEnabled());
	REQUIRE(tab.ui_spinPage->maximum() > 1);

	// Empty replies end both sources. Inspect the requested URLs after real Next.
	CustomNetworkAccessManager::NextFiles.enqueue(pagingReply(directory, "empty-first.json", 0));
	CustomNetworkAccessManager::NextFiles.enqueue(pagingReply(directory, "empty-second.json", 0));
	tab.nextPage();
	REQUIRE(QTest::qWaitFor([&tab]() { return tab.m_pendingPages.isEmpty() && tab.m_thumbnailsLoading.isEmpty(); }, 5000));
	REQUIRE(tab.ui_spinPage->value() == 2);
	const auto nextFirst = tab.m_pages.value(first->url()).last();
	const auto nextSecond = tab.m_pages.value(second->url()).last();
	REQUIRE(nextFirst->url().host() == first->url());
	REQUIRE(nextSecond->url().host() == second->url());
	REQUIRE(QUrlQuery(nextFirst->url()).queryItemValue("after") == "first-source");
	REQUIRE(QUrlQuery(nextSecond->url()).queryItemValue("after") == "second-source");
	REQUIRE(nextFirst->isLoaded());
	REQUIRE(nextSecond->isLoaded());
	REQUIRE(nextFirst->pageImageCount() == 0);
	REQUIRE(nextSecond->pageImageCount() == 0);
	REQUIRE(tab.m_failedPages.isEmpty());
	REQUIRE_FALSE(tab.ui_buttonNextPage->isEnabled());
	REQUIRE_FALSE(tab.m_endlessLoadingEnabled);
	REQUIRE(tab.m_images.isEmpty());
	REQUIRE(CustomNetworkAccessManager::NextFiles.isEmpty());
}
