#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QLinearGradient>
#include <QListWidget>
#include <QPainter>
#include <QScopedPointer>
#include <QTemporaryDir>
#include <QTreeWidget>
#include "models/library-importer.h"
#include "models/library-store.h"
#include "models/profile.h"
#include "tabs/library-tab.h"
#include "theme-loader.h"
#include "viewer/library-image-dialog.h"
#include "catch.h"
#include "source-helpers.h"

// Real widgets and local imports; generated demo artwork contains no personal data.
TEST_CASE("Library demo capture", "[.][library][demo]")
{
    QTemporaryDir directory;
    QTemporaryDir files(QDir::tempPath() + "/Grabber-Woo-Edit-Demo-XXXXXX");
    REQUIRE(files.isValid());
    const QScopedPointer<Profile> profile(makeLibraryProfile(directory.path()));
    QApplication::setStyle("Fusion");
    ThemeLoader theme(QFileInfo(QString::fromUtf8(__FILE__)).absolutePath() + "/../../dist/common/themes/", profile->getSettings());
    REQUIRE(theme.setTheme("Tokyo Night"));
    const auto collection = profile->library()->createCollection("Landscape studies");
    const QStringList names {"Amber ridge", "After the rain", "Blue hour", "Quiet shore", "Desert light", "Evening hills", "Alpine morning", "Moonlit valley"};
    QStringList keys;
    for (int i = 0; i < names.size(); ++i) {
        QImage image(640, 400, QImage::Format_RGB32);
        QPainter painter(&image);
        painter.setRenderHint(QPainter::Antialiasing);
        QLinearGradient sky(0, 0, 0, 400);
        sky.setColorAt(0, QColor::fromHsv((i * 34 + 205) % 360, 95, 115));
        sky.setColorAt(1, QColor::fromHsv((i * 34 + 245) % 360, 105, 225));
        painter.fillRect(image.rect(), sky);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor("#f5d5ad"));
        painter.drawEllipse(QPointF(470 - i * 24, 95), 30 + i * 2, 30 + i * 2);
        for (int layer = 0; layer < 3; ++layer) {
            painter.setBrush(QColor::fromHsv((i * 34 + 225) % 360, 110, 105 - layer * 23));
            QPolygonF ridge {QPointF(0, 400), QPointF(0, 250 + layer * 45), QPointF(170, 150 + layer * 55 + i * 4), QPointF(335, 260 + layer * 25), QPointF(505, 175 + layer * 52), QPointF(640, 255 + layer * 30), QPointF(640, 400)};
            painter.drawPolygon(ridge);
        }
        painter.end();
        const QString path = files.filePath(names[i] + ".png");
        REQUIRE(image.save(path));
        const auto imported = LibraryImporter::inspect(path);
        REQUIRE(imported.error.isEmpty());
        const QString key = profile->library()->saveLocalImage(imported);
        REQUIRE(profile->library()->addToCollection(key, collection));
        REQUIRE(profile->library()->setLiked(key, i % 2 == 0, collection));
        REQUIRE(profile->library()->setFavorite(key, i == 0 || i == 3, collection));
        keys.append(key);
    }
    REQUIRE(profile->library()->setCollectionCover(collection, keys[0]));
    REQUIRE(profile->library()->setNotes(keys[0], "Warm sky, cool shadows. Keep this palette for the next study.", collection));
    LibraryTab library(profile.data(), nullptr);
    library.resize(1280, 780);
    library.show();
    auto *sidebar = library.findChild<QTreeWidget*>("librarySidebar");
    sidebar->setCurrentItem(sidebar->topLevelItem(5)->child(0));
    QApplication::processEvents();
    auto *grid = library.findChild<QListWidget*>("libraryGrid");
    REQUIRE(grid->count() == 8);
    grid->item(0)->setSelected(true);
    QApplication::processEvents();
    const QString screenshot = qEnvironmentVariable("GRABBER_DEMO_SCREENSHOT");
    REQUIRE(!screenshot.isEmpty());
    REQUIRE(library.grab().save(screenshot));
    LibraryImageDialog viewer(profile.data(), keys, keys[0], collection);
    viewer.resize(1100, 760);
    viewer.show();
    QApplication::processEvents();
    REQUIRE(viewer.grab().save(qEnvironmentVariable("GRABBER_DEMO_VIEWER_SCREENSHOT")));
}
