#pragma once
#include <QtNetwork>
#include <functional>

struct HttpResult {
  QByteArray data;
  QString error;
  int status = 0;
};
struct HttpOptions {
  QByteArray form;
  QMap<QByteArray, QByteArray> headers;
  bool diskCache = true;
  int intervalMs = 1100;
};
// One bounded, cancellable queue shared by recording/release/art/lyrics
// requests.
class HttpClient : public QObject {
public:
  enum class Kind { Json, Image };
  using Callback = std::function<void(HttpResult)>;
  explicit HttpClient(QObject *parent = nullptr, QString cacheDirectory = {});
  ~HttpClient() override;
  void get(const QUrl &url, Kind kind, QObject *context, Callback callback,
           HttpOptions options = {});
  void cancel(QObject *context);
  void setContact(const QString &contact);
  static QString validate(const QByteArray &data, Kind kind);
  static constexpr qint64 ImageLimit = 20 * 1024 * 1024;
  static constexpr qint64 JsonLimit = 4 * 1024 * 1024;

private:
  struct Request {
    QUrl url;
    Kind kind;
    QPointer<QObject> context;
    Callback callback;
    HttpOptions options;
  };
  QNetworkAccessManager manager;
  QTimer timer;
  QElapsedTimer clock;
  QQueue<Request> queue;
  QPointer<QNetworkReply> active;
  QPointer<QObject> activeContext;
  QHash<QString, qint64> nextRequest;
  QString cache;
  QByteArray userAgent;
  QString cachePath(const Request &request) const;
  void pump();
};
