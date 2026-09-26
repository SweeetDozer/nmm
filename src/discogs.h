#pragma once
#include "online_dialogs.h"
struct DiscogsRelease {
  QString id, label;
};
QList<DiscogsRelease> discogsSearchResults(const QByteArray &data,
                                           QString &error);
QList<Candidate> discogsTracks(const QByteArray &data, QString &error);
QString discogsReleaseId(const QString &input);
class DiscogsService {
public:
  explicit DiscogsService(HttpClient &client,
                          QUrl base = QUrl("https://api.discogs.com/"))
      : http(client), base(std::move(base)) {}
  void search(QString title, QString artist, QString token, QObject *context,
              std::function<void(QList<DiscogsRelease>, QString)> done);
  void release(QString id, QString token, QObject *context,
               std::function<void(QList<Candidate>, QString)> done);
  void cancel(QObject *context) { http.cancel(context); }

private:
  HttpClient &http;
  QUrl base;
};
class DiscogsDialog : public QDialog {
public:
  DiscogsDialog(const Track &, DiscogsService &, QString token,
                QWidget *parent = nullptr);
  ~DiscogsDialog() override;
  std::optional<Change> proposal() const;
  QStringList journal() const { return events; }

private:
  Track track;
  DiscogsService &service;
  QString token;
  QObject *request = nullptr;
  QLineEdit *title, *artist, *id;
  QListWidget *releases, *tracks;
  QLabel *source;
  QPlainTextEdit *log;
  QPushButton *apply;
  QList<DiscogsRelease> releaseOptions;
  QList<Candidate> options;
  QStringList events;
  void reset();
  void message(const QString &text);
  void search();
  void load(const QString &id);
};
