#ifndef BACKUP_H
#define BACKUP_H

#include <QString>


class Profile;

/**
 * Generates a backup archive for the given profile.
 *
 * @param profile The profile to generate a backup for.
 * @param filePath The path of the destination ZIP file.
 * @return Whether the backup could be generated correctly.
 */
bool saveBackup(Profile *profile, const QString &filePath);

/**
 * Restore settings and data from the given backup archive.
 *
 * @param profile The profile in which the backup should be loaded.
 * @param filePath The path of the backup ZIP file.
 * @return Whether the backup could be restored correctly.
 */
bool loadBackup(Profile *profile, const QString &filePath);

/**
 * Copies `settings.ini` to `destination` without the legacy `Story/apiKey`, leaving every other setting
 * as it was. Returns false when the source cannot be read, the copy cannot be written, or the written
 * copy still contains the key.
 */
bool copySettingsWithoutStoryKey(const QString &source, const QString &destination);

#endif // BACKUP_H
