#pragma once
#include "core.h"
#include <QtNetwork>
struct Candidate {
  QMap<QString, QString> fields;
  QString source, reason;
  bool reliable = false;
};
QList<Candidate> musicBrainzCandidates(const Track &track,
                                       const QByteArray &data);
class MetadataSource : public QObject {
  Q_OBJECT
public:
  using QObject::QObject;
  virtual void lookup(const Track &track) = 0;
  virtual void cancel() = 0;
signals:
  void ready(const Track &track, const QList<Candidate> &candidates,
             const QString &error);
};
class MusicBrainz : public MetadataSource {
  Q_OBJECT
public:
  explicit MusicBrainz(QObject *parent = nullptr);
  void lookup(const Track &track) override;
  void cancel() override;
  void setContact(const QString &value) { contact = value; }

private:
  QNetworkAccessManager manager;
  QNetworkReply *active = nullptr;
  QTimer timer;
  QElapsedTimer elapsed;
  QString cache, contact;
  bool stopped = false;
  int delay = 1100;
  void finish(const Track &, const QByteArray &, const QString &error = {});
};
