#include "online.h"
namespace {
QString credits(const QJsonValue &value) {
  QString result;
  for (auto item : value.toArray()) {
    auto a = item.toObject();
    result += a.value("name").toString(
                  a.value("artist").toObject().value("name").toString()) +
              a["joinphrase"].toString();
  }
  return result;
}
bool mbid(const QString &id) {
  return QRegularExpression("^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-"
                            "9a-fA-F]{4}-[0-9a-fA-F]{12}$")
      .match(id)
      .hasMatch();
}
bool timedText(const QString &s) {
  return s.contains(QRegularExpression("\\[\\d{1,3}:\\d{2}(?:[.:]\\d+)?\\]"));
}
QString version(const QString &s) {
  QStringList words;
  auto lower = normalized(s);
  for (auto &word : QStringList{"remix", "mix", "live", "cover", "instrumental",
                                "acoustic", "edit", "radio", "sped", "slowed",
                                "karaoke", "ремикс", "кавер", "концерт"})
    if (lower.contains(
            QRegularExpression("\\b" + word + "\\b",
                               QRegularExpression::UseUnicodePropertiesOption)))
      words << word;
  return words.join(" ");
}
} // namespace
QMap<QString, QString> ReleaseOption::fields() const {
  QMap<QString, QString> result;
  const QMap<QString, QString> all = {{"ALBUM", album},
                                      {"ALBUMARTIST", albumArtist},
                                      {"DATE", date},
                                      {"TRACKNUMBER", trackNumber},
                                      {"DISCNUMBER", discNumber}};
  for (auto it = all.begin(); it != all.end(); ++it)
    if (!it.value().isEmpty())
      result[it.key()] = it.value();
  return result;
}
ReleasePage parseReleasePage(const QByteArray &data, const QString &recordingId,
                             int offset) {
  ReleasePage result;
  auto doc = QJsonDocument::fromJson(data);
  auto root = doc.object();
  if (!root["releases"].isArray() || !root["release-count"].isDouble()) {
    result.error = "Ответ MusicBrainz не содержит списка изданий";
    return result;
  }
  const auto releases = root["releases"].toArray();
  result.total = root["release-count"].toInt();
  result.nextOffset = offset + releases.size();
  for (auto entry : releases) {
    auto o = entry.toObject();
    ReleaseOption base;
    base.id = o["id"].toString();
    if (!mbid(base.id))
      continue;
    base.recordingId = recordingId;
    base.album = o["title"].toString();
    base.albumArtist = credits(o["artist-credit"]);
    base.date = o["date"].toString();
    base.country = o["country"].toString();
    base.status = o["status"].toString();
    base.disambiguation = o["disambiguation"].toString();
    auto rg = o["release-group"].toObject();
    QStringList types;
    if (!rg["primary-type"].toString().isEmpty())
      types << rg["primary-type"].toString();
    for (auto type : rg["secondary-types"].toArray())
      types << type.toString();
    base.type = types.join(" / ");
    bool found = false;
    for (auto medium : o["media"].toArray()) {
      auto m = medium.toObject();
      for (auto item : m["tracks"].toArray()) {
        auto track = item.toObject();
        if (track["recording"].toObject()["id"].toString() != recordingId)
          continue;
        auto option = base;
        option.trackNumber = track["number"].toString();
        if (option.trackNumber.isEmpty() && track["position"].toInt() > 0)
          option.trackNumber = QString::number(track["position"].toInt());
        if (m["position"].toInt() > 0)
          option.discNumber = QString::number(m["position"].toInt());
        option.format = m["format"].toString();
        result.options << option;
        found = true;
      }
    }
    // Browse relation proves membership, but a missing tracklist does not prove
    // a track number.
    if (!found)
      result.options << base;
  }
  if (releases.isEmpty() && offset < result.total)
    result.error =
        "Сервис вернул пустую страницу до конца списка; повторите позже";
  return result;
}
Change selectedProposal(const Track &t, const Candidate &candidate,
                        const std::optional<ReleaseOption> &release,
                        const std::optional<CoverOffer> &cover) {
  Change result{t, candidate.fields, {}, {}};
  for (auto it = result.fields.begin(); it != result.fields.end(); ++it)
    result.sources[it.key()] = candidate.source;
  if (!release || release->recordingId != candidate.recordingId)
    return result;
  const auto fields = release->fields();
  for (auto it = fields.begin(); it != fields.end(); ++it) {
    result.fields[it.key()] = it.value();
    result.sources[it.key()] =
        "MusicBrainz · https://musicbrainz.org/release/" + release->id;
  }
  if (cover && cover->releaseId == release->id && cover->error.isEmpty() &&
      !cover->absent && !cover->data.isEmpty()) {
    result.cover = cover->data;
    result.sources["PICTURE"] = cover->source;
  }
  return result;
}
QList<LyricOption> parseLyrics(const QByteArray &data, const Track &query) {
  QList<LyricOption> result;
  QSet<QString> compatibleIds;
  for (auto entry : QJsonDocument::fromJson(data).array()) {
    auto o = entry.toObject();
    if (o["instrumental"].toBool())
      continue;
    LyricOption base;
    base.id = o["id"].toVariant().toString();
    base.title = o["trackName"].toString();
    base.artist = o["artistName"].toString();
    base.album = o["albumName"].toString();
    base.duration = o["duration"].toDouble();
    if (base.id.isEmpty())
      continue;
    base.source = "LRCLIB · https://lrclib.net/api/get/" + base.id;
    bool title = !query.value("TITLE").isEmpty() &&
                 normalized(base.title) == normalized(query.value("TITLE"));
    bool artist = !query.value("ARTIST").isEmpty() &&
                  normalized(base.artist) == normalized(query.value("ARTIST"));
    bool length = query.duration > 0 && base.duration > 0 &&
                  qAbs(base.duration - query.duration / 1000.0) <= 2.0;
    bool album = query.value("ALBUM").isEmpty() ||
                 normalized(base.album) == normalized(query.value("ALBUM"));
    bool sameVersion = version(base.title) == version(query.value("TITLE"));
    auto fileVersion = version(cleanTitle(QFileInfo(query.path).fileName()));
    if (!fileVersion.isEmpty() && fileVersion != version(base.title))
      sameVersion = false;
    base.eligible = title && artist && length && album && sameVersion;
    base.reason = QString("Название: %1; исполнитель: %2; длительность: %3; "
                          "альбом: %4; версия: %5.")
                      .arg(title ? "совпадает" : "отличается",
                           artist ? "совпадает" : "отличается",
                           length ? "близка" : "не совпадает / неизвестна",
                           album ? "без конфликта" : "отличается",
                           sameVersion ? "без конфликта" : "КОНФЛИКТ");
    if (base.eligible)
      compatibleIds.insert(base.id);
    for (auto key : QStringList{"plainLyrics", "syncedLyrics"}) {
      auto item = base;
      item.text = o[key].toString();
      if (item.text.trimmed().isEmpty())
        continue;
      item.synced = key == "syncedLyrics" || timedText(item.text);
      if (item.synced) {
        item.eligible = false;
        item.reason += " Синхронизированный LRC: только просмотр; запись "
                       "SYLT/LRC в теги не реализована.";
      }
      result << item;
    }
  }
  if (compatibleIds.size() > 1)
    for (auto &item : result) {
      item.eligible = false;
      item.reason += " Неоднозначно: несколько подходящих версий. Уточните "
                     "альбом; запись заблокирована.";
    }
  return result;
}
void OnlineServices::releases(const QString &id, int offset, QObject *context,
                              std::function<void(ReleasePage)> done) {
  if (!mbid(id)) {
    QTimer::singleShot(0, context, [done] {
      ReleasePage p;
      p.error = "Некорректный идентификатор записи";
      done(p);
    });
    return;
  }
  auto url = urls.musicBrainz.resolved(QUrl("release"));
  QUrlQuery query;
  query.addQueryItem("recording", id);
  query.addQueryItem("inc", "artist-credits+recordings+release-groups+media");
  query.addQueryItem("limit", "25");
  query.addQueryItem("offset", QString::number(offset));
  query.addQueryItem("fmt", "json");
  url.setQuery(query);
  http.get(url, HttpClient::Kind::Json, context, [=](HttpResult r) {
    if (!r.error.isEmpty()) {
      ReleasePage p;
      p.error = r.error;
      done(p);
    } else
      done(parseReleasePage(r.data, id, offset));
  });
}
void OnlineServices::cover(const QString &id, QObject *context,
                           std::function<void(CoverOffer)> done) {
  if (!mbid(id)) {
    QTimer::singleShot(0, context, [done, id] {
      done({id, {}, {}, "Некорректный идентификатор издания", false});
    });
    return;
  }
  // Never use release-group art or a different release as fallback.
  auto url = urls.coverArt.resolved(QUrl("release/" + id + "/front"));
  http.get(url, HttpClient::Kind::Image, context, [=](HttpResult r) {
    CoverOffer offer;
    offer.releaseId = id;
    offer.source = "Cover Art Archive · " + url.toString();
    if (r.status == 404)
      offer.absent = true;
    else if (!r.error.isEmpty())
      offer.error = r.error;
    else
      offer.data = r.data;
    done(offer);
  });
}
void OnlineServices::lyrics(
    const Track &query, QObject *context,
    std::function<void(QList<LyricOption>, QString)> done) {
  auto url = urls.lyrics.resolved(QUrl("search"));
  QUrlQuery q;
  q.addQueryItem("track_name", query.value("TITLE"));
  q.addQueryItem("artist_name", query.value("ARTIST"));
  if (!query.value("ALBUM").isEmpty())
    q.addQueryItem("album_name", query.value("ALBUM"));
  url.setQuery(q);
  http.get(url, HttpClient::Kind::Json, context, [=](HttpResult r) {
    if (r.error.isEmpty() && !QJsonDocument::fromJson(r.data).isArray())
      r.error = "LRCLIB вернул некорректный список";
    done(r.error.isEmpty() ? parseLyrics(r.data, query) : QList<LyricOption>(),
         r.error);
  });
}
