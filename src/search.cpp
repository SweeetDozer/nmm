#include "search.h"
namespace {
QString quoted(QString s) {
  s.replace(QRegularExpression("[+\\-!(){}\\[\\]^\"~*?:\\\\/&|]"), " ");
  return "\"" + s.simplified() + "\"";
}
QString versions(QString s) {
  QStringList found;
  s = normalized(s);
  for (const auto &v : QStringList{
           "remix", "mix", "live", "cover", "instrumental", "acoustic", "edit",
           "radio", "sped", "slowed", "karaoke", "ремикс", "кавер", "концерт"})
    if (s.contains(QRegularExpression(
            "\\b" + v + "\\b", QRegularExpression::UseUnicodePropertiesOption)))
      found << v;
  return found.join(" ");
}
} // namespace
MusicBrainz::MusicBrainz(QObject *parent) : MetadataSource(parent) {
  cache = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) +
          "/musicbrainz";
  QDir().mkpath(cache);
  timer.setSingleShot(true);
  elapsed.start();
}
void MusicBrainz::cancel() {
  stopped = true;
  timer.stop();
  if (active)
    active->abort();
}
void MusicBrainz::lookup(const Track &t) {
  stopped = false;
  auto local = localSuggestion(t);
  QString title =
      t.value("TITLE").isEmpty() ? local["TITLE"] : t.value("TITLE");
  QString artist =
      t.value("ARTIST").isEmpty() ? local["ARTIST"] : t.value("ARTIST");
  auto queryFor = [](const QString &name, const QString &performer) {
    QString query = "recording:" + quoted(name);
    if (!performer.isEmpty())
      query += " AND artist:" + quoted(performer);
    return query;
  };
  QString query = queryFor(title, artist);
  const QString fromFile = queryFor(local["TITLE"], local["ARTIST"]);
  if (query != fromFile)
    query = "(" + query + ") OR (" + fromFile + ")";
  QString file =
      cache + "/" +
      QString::fromLatin1(
          QCryptographicHash::hash(query.toUtf8(), QCryptographicHash::Sha256)
              .toHex()) +
      ".json";
  QFile cached(file);
  if (QFileInfo(file).lastModified().secsTo(QDateTime::currentDateTime()) <
          30 * 86400 &&
      cached.open(QIODevice::ReadOnly)) {
    auto data = cached.readAll();
    QTimer::singleShot(0, this, [=, this] {
      if (!stopped)
        finish(t, data);
    });
    return;
  }
  timer.disconnect(this);
  connect(&timer, &QTimer::timeout, this, [=, this] {
    QUrl url("https://musicbrainz.org/ws/2/recording/");
    QUrlQuery q;
    q.addQueryItem("query", query);
    q.addQueryItem("fmt", "json");
    q.addQueryItem("limit", "5");
    url.setQuery(q);
    QNetworkRequest request(url);
    request.setRawHeader("User-Agent",
                         ("MusicOrder/0.1.0 (" + contact + ")").toUtf8());
    request.setTransferTimeout(20000);
    elapsed.restart();
    active = manager.get(request);
    connect(active, &QNetworkReply::finished, this, [=, this] {
      auto reply = active;
      active = nullptr;
      QByteArray data = reply->readAll();
      QString error;
      if (reply->error() != QNetworkReply::NoError)
        error = reply->errorString();
      int status =
          reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
      if (status == 429 || status == 503) {
        bool valid = false;
        int seconds = reply->rawHeader("Retry-After").toInt(&valid);
        delay = valid ? int(qBound(qint64(1100), qint64(seconds) * 1000,
                                   qint64(86400000)))
                      : 60000;
        elapsed.restart();
      } else
        delay = 1100;
      if (error.isEmpty()) {
        QJsonParseError pe;
        QJsonDocument::fromJson(data, &pe);
        if (pe.error != QJsonParseError::NoError)
          error = "Неверный JSON сервера";
      }
      if (error.isEmpty()) {
        QSaveFile f(file);
        if (f.open(QIODevice::WriteOnly)) {
          f.write(data);
          f.commit();
        }
      }
      reply->deleteLater();
      if (!stopped)
        finish(t, data, error);
    });
  });
  timer.start(qMax(0, delay - int(elapsed.elapsed())));
}
QList<Candidate> musicBrainzCandidates(const Track &t, const QByteArray &data) {
  QList<Candidate> out;
  auto local = localSuggestion(t);
  if (!data.isEmpty()) {
    auto recordings =
        QJsonDocument::fromJson(data).object()["recordings"].toArray();
    for (const auto &entry : recordings) {
      auto o = entry.toObject();
      Candidate c;
      c.fields["TITLE"] = o["title"].toString();
      QStringList artists;
      for (const auto &ac : o["artist-credit"].toArray()) {
        auto a = ac.toObject();
        artists << a["name"].toString() + a["joinphrase"].toString();
      }
      c.fields["ARTIST"] = artists.join("");
      c.source = "MusicBrainz · " + o["id"].toString();
      QString title = t.value("TITLE").isEmpty() ? local["TITLE"]
                                                 : t.value("TITLE"),
              artist = t.value("ARTIST").isEmpty() ? local["ARTIST"]
                                                   : t.value("ARTIST");
      bool titleMatch = !title.isEmpty() &&
                        normalized(title) == normalized(c.fields["TITLE"]),
           artistMatch = !artist.isEmpty() &&
                         normalized(artist) == normalized(c.fields["ARTIST"]);
      const bool fileTitleMatch =
          !local["TITLE"].isEmpty() &&
          normalized(local["TITLE"]) == normalized(c.fields["TITLE"]);
      const bool folderArtistMatch =
          !local["ARTIST"].isEmpty() &&
          normalized(local["ARTIST"]) == normalized(c.fields["ARTIST"]);
      int length = o["length"].toInt();
      bool duration =
          length > 0 && t.duration > 0 && qAbs(length - t.duration) <= 2000;
      bool version =
          versions(title) ==
          versions(c.fields["TITLE"] + " " + o["disambiguation"].toString());
      // Conflicting explicit version markers in either tags or filename
      // prohibit bulk recommendation.
      const auto fileVersion = versions(local["TITLE"]);
      if (!fileVersion.isEmpty() &&
          fileVersion != versions(c.fields["TITLE"] + " " +
                                  o["disambiguation"].toString()))
        version = false;
      c.reliable = ((titleMatch && artistMatch) ||
                    (fileTitleMatch && folderArtistMatch)) &&
                   duration && version;
      c.reason =
          QString("Название: %1; исполнитель: %2; длительность: %3; версия: "
                  "%4. %5 изданий — альбом не выбран.")
              .arg(titleMatch ? "совпадает" : "отличается",
                   artistMatch ? "совпадает" : "отличается",
                   length > 0
                       ? QString::number(length / 1000.0, 'f', 1) + " с (Δ " +
                             QString::number(qAbs(length - t.duration) / 1000.0,
                                             'f', 1) +
                             " с)"
                       : "неизвестна",
                   version ? "без выявленного конфликта" : "КОНФЛИКТ")
              .arg(o["releases"].toArray().size());
      c.reason += QString(" Имя файла: %1; папка исполнителя: %2.")
                      .arg(fileTitleMatch ? "совпадает" : "отличается",
                           folderArtistMatch ? "совпадает" : "отличается");
      out << c;
    }
  }
  int reliable = 0;
  for (const auto &c : out)
    reliable += c.reliable;
  if (reliable > 1)
    for (auto &c : out) {
      c.reliable = false;
      c.reason += " Неоднозначно: несколько совпадений.";
    }
  Candidate fallback;
  fallback.fields = local;
  fallback.source = "Папка и имя файла";
  fallback.reason = "Локальная подсказка, требует проверки. Альбом, год и "
                    "обложка неизвестны.";
  out << fallback;
  return out;
}

void MusicBrainz::finish(const Track &t, const QByteArray &data,
                         const QString &error) {
  emit ready(t, musicBrainzCandidates(t, error.isEmpty() ? data : QByteArray()),
             error);
}
