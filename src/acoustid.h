#pragma once
#include "search.h"
struct Fingerprint {
  QString value, error;
  int duration = 0;
};
Fingerprint parseFingerprint(const QByteArray &data);
QList<Candidate> acoustIdCandidates(const QByteArray &data, QString &error,
                                    QStringList &diagnostics);
class AcoustId : public MetadataSource {
  Q_OBJECT
public:
  explicit AcoustId(HttpClient &http, QObject *parent = nullptr,
                    QUrl endpoint = QUrl("https://api.acoustid.org/v2/lookup"));
  ~AcoustId() override;
  void configure(QString key, QString executable);
  void lookup(const Track &track) override;
  void cancel() override;

private:
  HttpClient &http;
  QUrl endpoint;
  QProcess process;
  QTimer timeout;
  QString key, executable;
  Track current;
  bool running = false;
  quint64 generation = 0;
  QByteArray output;
  QString processError;
  void fail(const QString &error);
  void calculated(int exitCode, QProcess::ExitStatus status);
};
