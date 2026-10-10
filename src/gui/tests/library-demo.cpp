#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QItemSelectionModel>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLinearGradient>
#include <QPainter>
#include <QRandomGenerator>
#include <QScopedPointer>
#include <QScrollBar>
#include <QScopeGuard>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>
#include <QTreeWidget>
#include "custom-network-access-manager.h"
#include "discovery-feed.h"
#include "models/image.h"
#include "models/library-importer.h"
#include "models/library-store.h"
#include "models/profile.h"
#include "models/site.h"
#include "tabs/home-tab.h"
#include "tabs/library-tab.h"
#include "tags/tag.h"
#include "theme-loader.h"
#include "ui/image-grid.h"
#include "viewer/library-image-dialog.h"
#include "catch.h"
#include "source-helpers.h"

namespace
{
	QImage landscape(int seed, int width, int height)
	{
		QImage image(width, height, QImage::Format_RGB32);
		QPainter painter(&image);
		painter.setRenderHint(QPainter::Antialiasing);
		QLinearGradient sky(0, 0, 0, height);
		sky.setColorAt(0, QColor::fromHsv((seed * 34 + 205) % 360, 95, 115));
		sky.setColorAt(1, QColor::fromHsv((seed * 34 + 245) % 360, 105, 225));
		painter.fillRect(image.rect(), sky);
		painter.setPen(Qt::NoPen);
		painter.setBrush(QColor("#f5d5ad"));
		painter.drawEllipse(QPointF(width * 0.72 - seed * 9 % (width / 3), height * 0.22), width * 0.06 + seed % 4 * 2, width * 0.06 + seed % 4 * 2);
		QRandomGenerator rng(quint32(seed) * 7919u + 17u);
		for (int layer = 0; layer < 3; ++layer) {
			painter.setBrush(QColor::fromHsv((seed * 34 + 225 + layer * 15) % 360, 90 + int(rng.bounded(60)), 120 - layer * 28));
			QPolygonF ridge {QPointF(0, height)};
			const int peaks = 3 + int(rng.bounded(4));
			for (int peak = 0; peak <= peaks; ++peak) {
				ridge.append(QPointF(width * peak / double(peaks), height * (0.35 + layer * 0.13 + rng.generateDouble() * 0.25)));
			}
			ridge.append(QPointF(width, height));
			painter.drawPolygon(ridge);
		}
		return image;
	}
}

// Real widgets with generated artwork: no personal Library content is captured.
TEST_CASE("Library demo capture", "[.][library][demo]")
{
	QTemporaryDir directory;
	QTemporaryDir files(QDir::tempPath() + "/Grabber-Woo-Edit-Demo-XXXXXX");
	REQUIRE(files.isValid());
	const QScopedPointer<Profile> profile(makeLibraryProfile(directory.path()));
	QApplication::setStyle("Fusion");
	ThemeLoader theme(QFileInfo(QString::fromUtf8(__FILE__)).absolutePath() + "/../../dist/common/themes/", profile->getSettings());
	REQUIRE(theme.setTheme("Woo Night"));
	const auto collection = profile->library()->createCollection("Landscape studies");
	QStringList keys;
	for (int i = 0; i < 18; ++i) {
		const QString path = files.filePath(QString("Study %1.png").arg(i));
		REQUIRE(landscape(i, 640, 800).save(path));
		const auto imported = LibraryImporter::inspect(path);
		REQUIRE(imported.error.isEmpty());
		const QString key = profile->library()->saveLocalImage(imported);
		REQUIRE(profile->library()->addToCollection(key, collection));
		REQUIRE((i % 3 == 0 ? profile->library()->setFavorite(key, true, collection) : profile->library()->setLiked(key, true, collection)));
		keys.append(key);
	}
	REQUIRE(profile->library()->setCollectionCover(collection, keys[0]));
	LibraryTab library(profile.data(), nullptr);
	library.resize(1440, 860);
	library.show();
	auto *sidebar = library.findChild<QTreeWidget*>("librarySidebar");
	sidebar->setCurrentItem(sidebar->topLevelItem(9)->child(0));
	QApplication::processEvents();
	auto *grid = library.grid();
	REQUIRE(grid->gridModel()->rowCount() == 18);
	grid->selectionModel()->select(grid->gridModel()->index(4), QItemSelectionModel::ClearAndSelect);
	QApplication::processEvents();
	CAPTURE(grid->width(), grid->viewport()->width(), grid->gridSize().width(), grid->tileSize().width(), grid->verticalScrollBar()->isVisible(), grid->verticalScrollBar()->width());
	REQUIRE(grid->viewport()->width() - (grid->viewport()->width() / grid->gridSize().width()) * grid->gridSize().width() < grid->gridSize().width());
	const int perRow = grid->viewport()->width() / grid->gridSize().width();
	CAPTURE(perRow, grid->visualRect(grid->gridModel()->index(perRow - 1)).y(), grid->visualRect(grid->gridModel()->index(0)).y());
	REQUIRE(grid->visualRect(grid->gridModel()->index(perRow - 1)).y() == grid->visualRect(grid->gridModel()->index(0)).y());
	const QString screenshot = qEnvironmentVariable("GRABBER_DEMO_SCREENSHOT");
	REQUIRE(!screenshot.isEmpty());
	REQUIRE(library.grab().save(screenshot));
	LibraryImageDialog viewer(profile.data(), keys, keys[0], collection);
	viewer.resize(1100, 760);
	viewer.show();
	QApplication::processEvents();
	REQUIRE(viewer.grab().save(qEnvironmentVariable("GRABBER_DEMO_VIEWER_SCREENSHOT")));
}

TEST_CASE("Discover demo capture", "[.][home][demo]")
{
	auto clearReplies = qScopeGuard([]() { CustomNetworkAccessManager::NextFiles.clear(); });
	QTemporaryDir directory;
	REQUIRE(directory.isValid());
	const QScopedPointer<Profile> profile(makeLibraryProfile(directory.path()));
	QApplication::setStyle("Fusion");
	ThemeLoader theme(QFileInfo(QString::fromUtf8(__FILE__)).absolutePath() + "/../../dist/common/themes/", profile->getSettings());
	REQUIRE(theme.setTheme("Woo Night"));
	auto *site = profile->getSites().value("danbooru.donmai.us");
	site->setSetting("sources/usedefault", false, true);
	site->setSetting("sources/source_1", "Json", "Xml");
	site->setSetting("sources/source_2", "", "Json");
	site->setSetting("sources/source_3", "", "Regex");
	site->setSetting("sources/source_4", "", "Rss");
	site->loadConfig();
	profile->getSettings()->setValue("sites", QStringList{site->url()});
	QFile state(DiscoveryFeed::statePath(profile->getPath()));
	REQUIRE(state.open(QIODevice::WriteOnly));
	const QJsonObject background {{site->url(), QJsonObject {{"posts", 400}, {"tags", QJsonObject {{"landscape", 8}}}}}};
	state.write(QJsonDocument(QJsonObject {{"version", 2}, {"background", background}}).toJson());
	state.close();
	for (int id = 1; id <= 4; ++id) {
		Image image(site, {{"id", QString::number(id)}, {"file_url", QString("https://test.invalid/%1.png").arg(id)}, {"tags", "landscape mountains"}}, profile.data());
		image.setTags({Tag("landscape", "general")});
		image.setPreviewImage(QPixmap::fromImage(landscape(id, 320, 400)));
		const QString key = profile->library()->saveImage(image);
		REQUIRE((id == 1 ? profile->library()->setFavorite(key, true) : profile->library()->setLiked(key, true)));
	}
	QJsonArray posts;
	for (int i = 0; i < 24; ++i) {
		posts.append(QJsonObject {{"id", 100 + i}, {"md5", QString("%1").arg(100 + i, 32, 10, QChar('0'))},
			{"file_url", QString("https://test.invalid/p%1.png").arg(i)}, {"tag_string", "landscape mountains lake"}, {"image_width", 800}, {"image_height", 1000}});
	}
	QFile response(directory.filePath("posts.json"));
	REQUIRE(response.open(QIODevice::WriteOnly));
	response.write(QJsonDocument(posts).toJson());
	response.close();
	CustomNetworkAccessManager::NextFiles.enqueue(response.fileName());
	for (int i = 0; i < 24; ++i) {
		const QString path = directory.filePath(QString("preview%1.png").arg(i));
		REQUIRE(landscape(i + 7, 320, 400).save(path));
		CustomNetworkAccessManager::NextFiles.enqueue(path);
	}
	HomeTab home(profile.data(), nullptr);
	home.resize(1440, 900);
	home.show();
	const bool filled = QTest::qWaitFor([&home]() { return home.grid()->gridModel()->rowCount() >= 12; }, 10000);
	CAPTURE(home.grid()->gridModel()->rowCount(), home.findChild<QLabel*>("homeStatus")->text().toStdString(), CustomNetworkAccessManager::NextFiles.size());
	REQUIRE(filled);
	QTest::qWait(400); // Let the fade-in finish.
	home.grid()->selectionModel()->select(home.grid()->gridModel()->index(2), QItemSelectionModel::ClearAndSelect);
	QApplication::processEvents();
	REQUIRE(home.grab().save(qEnvironmentVariable("GRABBER_DEMO_HOME_SCREENSHOT")));
}
