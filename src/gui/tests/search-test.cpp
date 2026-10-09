#include <QApplication>
#include <QComboBox>
#include <QScopedPointer>
#include <QTemporaryDir>
#include "models/image.h"
#include "models/profile.h"
#include "models/site.h"
#include "tabs/tag-tab.h"
#include "catch.h"
#include "source-helpers.h"

namespace {
class SearchFixture : public TagTab
{
	public:
		using TagTab::TagTab;
		using SearchTab::m_images;
		using SearchTab::clear;
};
}

TEST_CASE("Merged search keeps edits and stable image bindings", "[search][merge]")
{
	QTemporaryDir directory;
	REQUIRE(directory.isValid());
	const QScopedPointer<Profile> profile(makeLibraryProfile(directory.path()));
	auto *site = profile->getSites().value("danbooru.donmai.us");
	REQUIRE(site != nullptr);
	SearchFixture search(profile.data(), nullptr, nullptr);
	auto picture = [&](const QString &id, const QString &md5, const QString &url) {
		return QSharedPointer<Image>::create(site, QMap<QString, QString>{{"id", id}, {"md5", md5}, {"file_url", url}}, profile.data());
	};
	const QString a = "0123456789abcdef0123456789abcdef", b = "1123456789abcdef0123456789abcdef";
	auto original = picture("1", a, "https://test.invalid/original.png");
	auto same = picture("2", a.toUpper(), "https://mirror.invalid/copy.png");
	auto edit = picture("1", b, "https://test.invalid/original.png");
	auto noHash = picture("3", "", "https://test.invalid/other.png?variant=1");
	auto sameUrl = picture("3", "", "https://test.invalid/other.png?variant=1");
	auto variant = picture("3", "", "https://test.invalid/other.png?variant=2");
	auto unknown = picture("4", "not-a-checksum", "");
	const auto merged = search.mergeResults(1, {original, same, edit, noHash, sameUrl, variant, unknown, unknown});
	REQUIRE(merged == QList<QSharedPointer<Image>>{original, edit, noHash, variant, unknown, unknown});
	search.m_images = merged;
	REQUIRE(search.mergeResults(2, {same}).isEmpty());
	REQUIRE(search.m_images.first() == original); // Richer metadata must not replace a widget's bound pointer.
	search.clear();
	REQUIRE(search.mergeResults(1, {same}).size() == 1); // Same-query reload cannot inherit hidden hashes.
}
