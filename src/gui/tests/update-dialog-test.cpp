#include <QSignalSpy>
#include "custom-network-access-manager.h"
#include "updater/update-dialog.h"
#include "catch.h"

TEST_CASE("A failed update check does not block desktop startup")
{
    bool shouldQuit = false;
    UpdateDialog dialog(&shouldQuit);
    CustomNetworkAccessManager::NextFiles.enqueue("500");
    QSignalSpy rejected(&dialog, &QDialog::rejected);
    dialog.checkForUpdates();
    REQUIRE(rejected.wait());
    REQUIRE_FALSE(shouldQuit);
}
