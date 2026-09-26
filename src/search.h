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
struct SearchQuery {
  QString label, query;
};
QList<SearchQuery> musicBrainzQueries(const Track &track);
class MetadataSource : public QObject {
  Q_OBJECT
public:
  using QObject::QObject;
  virtual void lookup(const Track &track) = 0;
  virtual void cancel() = 0;
signals:
  void diagnostic(const QString &message);
  void ready(const Track &track, const QList<Candidate> &candidates,
             const QString &error);
};
class MusicBrainz : public MetadataSource {
  Q_OBJECT
public:
  explicit MusicBrainz(
      QObject *parent = nullptr, HttpClient *client = nullptr,
      QUrl endpoint = QUrl("https://musicbrainz.org/ws/2/recording/"));
  void lookup(const Track &track) override;
  void cancel() override;
  void setContact(const QString &value) { http->setContact(value); }

private:
  HttpClient *http;
  QUrl endpoint;
  Track current;
  QList<SearchQuery> queries;
  QList<Candidate> results;
  QStringList errors;
  quint64 generation = 0;
  int queryIndex = 0;
  void next(quint64 run);
  void complete();
};
