#pragma once
#include "core.h"
#include "http.h"
struct Candidate {
  QMap<QString, QString> fields;
  QString source, reason;
  bool reliable = false;
  QString recordingId;
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
  explicit MusicBrainz(QObject *parent = nullptr, HttpClient *client = nullptr);
  void lookup(const Track &track) override;
  void cancel() override;
  void setContact(const QString &value) { http->setContact(value); }

private:
  HttpClient *http;
};
