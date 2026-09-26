#include "http.h"
#include <QImage>
#include <QImageReader>
#include <limits>

HttpClient::HttpClient(QObject *parent, QString directory) : QObject(parent) {
  cache =
      directory.isEmpty()
          ? QStandardPaths::writableLocation(QStandardPaths::CacheLocation) +
                "/online-v2"
          : directory;
  QDir().mkpath(cache);
  clock.start();
  timer.setSingleShot(true);
  connect(&timer, &QTimer::timeout, this, [this] { pump(); });
  setContact("https://github.com/SweeetDozer/nmm");
}
HttpClient::~HttpClient() {
  timer.stop();
  if (active) {
    active->disconnect(this);
    active->abort();
  }
}
void HttpClient::setContact(const QString &contact) {
  userAgent = ("MusicOrder/0.2.0 (" + contact + ")").toUtf8();
}
QString HttpClient::cachePath(const Request &r) const {
  return cache + "/" +
         QString::fromLatin1(
             QCryptographicHash::hash(r.url.toEncoded() +
                                          QByteArray::number(int(r.kind)),
                                      QCryptographicHash::Sha256)
                 .toHex());
}
QString HttpClient::validate(const QByteArray &data, Kind kind) {
  if (data.size() > (kind == Kind::Image ? ImageLimit : JsonLimit))
    return "Превышен лимит загрузки";
  if (kind == Kind::Json) {
    QJsonParseError error;
    auto doc = QJsonDocument::fromJson(data, &error);
    return error.error == QJsonParseError::NoError && !doc.isNull()
               ? QString()
               : "Некорректный JSON сервиса";
  }
  QBuffer buffer;
  buffer.setData(data);
  buffer.open(QIODevice::ReadOnly);
  QImageReader reader(&buffer);
  auto format = reader.format().toLower();
  auto size = reader.size();
  if (format != "jpeg" && format != "png")
    return "Поддерживаются только JPEG и PNG; изображение не предложено";
  if (!size.isValid() || size.width() > 10000 || size.height() > 10000 ||
      qint64(size.width()) * size.height() > 25000000)
    return "Изображение превышает 25 мегапикселей / 10000 пикселей по стороне";
  if (reader.read().isNull())
    return "Изображение повреждено";
  return {};
}
void HttpClient::get(const QUrl &url, Kind kind, QObject *context,
                     Callback callback) {
  // HTTP is accepted only for loopback fixture servers. Production endpoints
  // use HTTPS.
  if (!url.isValid() ||
      (url.scheme() != "https" &&
       !(url.scheme() == "http" &&
         (url.host() == "127.0.0.1" || url.host() == "localhost")))) {
    QTimer::singleShot(0, context, [callback] {
      callback({{}, "Небезопасный адрес сервиса", 0});
    });
    return;
  }
  queue.enqueue({url, kind, context, std::move(callback)});
  if (!active)
    timer.start(0);
}
void HttpClient::cancel(QObject *context) {
  for (qsizetype i = queue.size(); i-- > 0;)
    if (queue[i].context == context)
      queue.removeAt(i);
  if (active && activeContext == context) {
    activeContext.clear();
    active->abort();
  }
}
void HttpClient::pump() {
  if (active)
    return;
  while (!queue.isEmpty() && !queue.head().context)
    queue.dequeue();
  if (queue.isEmpty())
    return;
  auto r = queue.head();
  auto path = cachePath(r);
  QFile f(path);
  const auto age =
      QFileInfo(f).lastModified().secsTo(QDateTime::currentDateTime());
  const qint64 limit = r.kind == Kind::Image ? ImageLimit : JsonLimit;
  if (age >= 0 && age < 30 * 86400 && f.size() <= limit &&
      f.open(QIODevice::ReadOnly)) {
    auto data = f.readAll();
    f.close();
    if (validate(data, r.kind).isEmpty()) {
      queue.dequeue();
      timer.start(0);
      r.callback({data, {}, 200});
      return;
    }
    QFile::remove(path);
  }
  auto wait = nextRequest.value(r.url.host()) - clock.elapsed();
  if (wait > 0) {
    timer.start(int(qMin<qint64>(wait, 60000)));
    return;
  }
  queue.dequeue();
  if (!r.context) {
    timer.start(0);
    return;
  }
  nextRequest[r.url.host()] = clock.elapsed() + 1100;
  QNetworkRequest request(r.url);
  request.setRawHeader("User-Agent", userAgent);
  request.setTransferTimeout(20000);
  request.setMaximumRedirectsAllowed(5);
  request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                       QNetworkRequest::NoLessSafeRedirectPolicy);
  auto reply = manager.get(request);
  active = reply;
  activeContext = r.context;
  reply->setReadBufferSize(64 * 1024);
  auto body = std::make_shared<QByteArray>();
  auto failure = std::make_shared<QString>();
  auto drain = [reply, body, failure, limit] {
    if (!failure->isEmpty())
      return;
    body->append(reply->read(qMax<qint64>(1, limit - body->size() + 1)));
    if (body->size() > limit) {
      *failure = "Превышен лимит загрузки";
      reply->abort();
    }
  };
  connect(reply, &QNetworkReply::readyRead, this, drain);
  connect(
      reply, &QNetworkReply::metaDataChanged, this, [reply, limit, failure] {
        if (reply->header(QNetworkRequest::ContentLengthHeader).toLongLong() >
            limit) {
          *failure = "Превышен лимит загрузки";
          reply->abort();
        }
      });
  connect(
      reply, &QNetworkReply::finished, this,
      [this, reply, r, path, body, failure, drain] {
        if (reply->isOpen())
          drain();
        HttpResult result;
        result.status =
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        result.error = *failure;
        if (result.error.isEmpty() && reply->error() != QNetworkReply::NoError)
          result.error = reply->errorString();
        if (result.error.isEmpty() && result.status != 200)
          result.error = "HTTP " + QString::number(result.status);
        if (result.error.isEmpty())
          result.error = validate(*body, r.kind);
        if (result.status == 429 || result.status == 503) {
          bool ok = false;
          auto seconds = reply->rawHeader("Retry-After").toLongLong(&ok);
          if (!ok) {
            auto date = QDateTime::fromString(
                QString::fromLatin1(reply->rawHeader("Retry-After")),
                Qt::RFC2822Date);
            seconds = date.isValid()
                          ? QDateTime::currentDateTimeUtc().secsTo(date)
                          : 60;
          }
          const auto now = clock.elapsed();
          const auto maxSeconds =
              (std::numeric_limits<qint64>::max() - now) / 1000;
          nextRequest[r.url.host()] =
              now +
              qMax(qint64(1100), qBound(qint64(0), seconds, maxSeconds) * 1000);
        }
        bool deliver = r.context && activeContext == r.context;
        active.clear();
        activeContext.clear();
        reply->deleteLater();
        timer.start(0);
        if (!deliver)
          return;
        if (result.error.isEmpty()) {
          result.data = *body;
          QSaveFile f(path);
          if (f.open(QIODevice::WriteOnly)) {
            f.write(result.data);
            f.commit();
          }
        }
        r.callback(std::move(result));
      });
}
