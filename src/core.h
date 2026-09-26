#pragma once
#include <QtCore>
#include <atomic>
#include <optional>
struct Track {
  QString path, format, error;
  QMap<QString, QStringList> tags;
  QByteArray cover;
  QStringList unsupported, complexKeys;
  int pictureCount = 0;
  bool coverLoaded = true;
  qint64 size = 0, modified = 0;
  int duration = 0, bitrate = 0, sampleRate = 0, channels = 0;
  QString value(const QString &key) const { return tags.value(key).join("; "); }
};
struct Change {
  Track before;
  QMap<QString, QString> fields;
  std::optional<QByteArray> cover;
  QMap<QString, QString> sources;
};
struct WriteResult {
  QString path, backup, error;
  Track after;
};
Track readTrack(const QString &path, bool loadCover = true);
WriteResult writeTrack(const Change &change, const QString &backupRoot);
QByteArray fileHash(const QString &path, std::atomic_bool *cancel = nullptr);
QString quarantineFile(const Track &track, const QString &root, QString &error);
QString cleanTitle(QString name);
QString normalized(QString text);
QMap<QString, QString> localSuggestion(const Track &track);
QStringList editableFields();
