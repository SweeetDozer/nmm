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
QList<SearchQuery> musicBrainzQueries(const Track &t) {
  QList<SearchQuery> out;
  const auto local = localSuggestion(t);
  const auto title = cleanSearchText(t.value("TITLE")),
             artist = cleanSearchText(t.value("ARTIST"));
  auto add = [&](QString label, QString name, QString performer) {
    if (!meaningfulSearchText(name))
      return;
    QString query = "recording:" + quoted(name);
    if (meaningfulSearchText(performer))
      query += " AND artist:" + quoted(performer);
    for (auto &previous : out)
      if (previous.query == query)
        return;
    if (out.size() < 4)
      out << SearchQuery{label, query};
  };
  add("Теги", title, artist);
  add("Имя файла / папка", local.value("TITLE"), local.value("ARTIST"));
  add("Название из файла без исполнителя", local.value("TITLE"), {});
  add("Название из тегов без исполнителя", title, {});
  return out;
}
MusicBrainz::MusicBrainz(QObject *parent, HttpClient *client, QUrl endpoint)
    : MetadataSource(parent), endpoint(std::move(endpoint)) {
  http = client ? client : new HttpClient(this);
}
void MusicBrainz::cancel() {
  ++generation;
  http->cancel(this);
}
void MusicBrainz::lookup(const Track &t) {
  cancel();
  current = t;
  queries = musicBrainzQueries(t);
  queryIndex = 0;
  results.clear();
  errors.clear();
  emit diagnostic("MusicBrainz · " + QFileInfo(t.path).fileName() +
                  " · исходные теги: " + t.value("ARTIST") + " — " +
                  t.value("TITLE"));
  if (!meaningfulSearchText(cleanSearchText(t.value("TITLE"))))
    emit diagnostic(
        "Тег TITLE пустой / числовой / служебный: не использован для запроса.");
  if (!meaningfulSearchText(cleanSearchText(t.value("ARTIST"))))
    emit diagnostic("Тег ARTIST пустой / числовой / служебный: ограничение по "
                    "нему не используется.");
  if (queries.isEmpty())
    emit diagnostic("Нет осмысленного названия для текстового поиска. Запросы "
                    "не отправлены; это НЕ отсутствие записи в каталоге. "
                    "Уточните теги или используйте AcoustID.");
  const auto run = generation;
  QTimer::singleShot(0, this, [this, run] { next(run); });
}
void MusicBrainz::complete() {
  int strong = 0;
  for (auto &c : results)
    strong += c.reliable;
  if (strong > 1)
    for (auto &c : results) {
      c.reliable = false;
      c.reason += " Несколько подходящих записей: требуется ручной выбор.";
    }
  const int count = results.size();
  auto local = musicBrainzCandidates(current, {});
  results.append(local);
  emit diagnostic(QString("MusicBrainz · итог: %1 записей каталога; локальных "
                          "подсказок %2; ошибок %3. Ничего не записано.")
                      .arg(count)
                      .arg(local.size())
                      .arg(errors.size()));
  emit ready(current, results, errors.join("\n"));
}
void MusicBrainz::next(quint64 run) {
  if (run != generation)
    return;
  if (queryIndex >= queries.size()) {
    complete();
    return;
  }
  const auto query = queries[queryIndex++];
  QUrl url = endpoint;
  QUrlQuery params;
  params.addQueryItem("query", query.query);
  params.addQueryItem("fmt", "json");
  params.addQueryItem("limit", "10");
  url.setQuery(params);
  emit diagnostic(QString("MusicBrainz · запрос %1/%2 · %3: %4")
                      .arg(queryIndex)
                      .arg(queries.size())
                      .arg(query.label, query.query));
  http->get(
      url, HttpClient::Kind::Json, this, [this, run, query](HttpResult reply) {
        if (run != generation)
          return;
        auto document = QJsonDocument::fromJson(reply.data);
        if (reply.error.isEmpty() && !document.object()["recordings"].isArray())
          reply.error = "Ответ не содержит списка записей";
        if (!reply.error.isEmpty()) {
          const auto error =
              "MusicBrainz · ОШИБКА запроса (не «не найдено»): " + reply.error;
          emit diagnostic(error);
          errors << error;
          complete();
          return;
        }
        const auto raw = document.object()["recordings"].toArray();
        emit diagnostic(
            QString("MusicBrainz · получено %1 из %2 результатов сервиса (до "
                    "10 на запрос).")
                .arg(raw.size())
                .arg(document.object().value("count").toInt(raw.size())));
        auto parsed = musicBrainzCandidates(current, reply.data);
        int displayed = 0, duplicates = 0, invalid = 0;
        for (auto &c : parsed) {
          if (!c.source.startsWith("MusicBrainz"))
            continue;
          if (c.fields.value("TITLE").trimmed().isEmpty()) {
            ++invalid;
            emit diagnostic(
                "Отклонён повреждённый кандидат: отсутствует название.");
            continue;
          }
          bool duplicate = false;
          for (auto &old : results)
            if (!c.recordingId.isEmpty() && old.recordingId == c.recordingId) {
              duplicate = true;
              break;
            }
          if (duplicate) {
            ++duplicates;
            continue;
          }
          emit diagnostic(
              c.fields.value("ARTIST") + " — " + c.fields.value("TITLE") +
              " · " +
              (c.reliable ? "Сильное совпадение: "
                          : "Не подтверждён, ПОКАЗАН для ручного выбора: ") +
              c.reason);
          c.reason += " Запрос: " + query.query;
          results << c;
          ++displayed;
        }
        emit diagnostic(QString("Показано новых: %1; объединено повторов MBID: "
                                "%2; повреждённых: %3. Ноль результатов — "
                                "ответ каталога, не сетевая ошибка.")
                            .arg(displayed)
                            .arg(duplicates)
                            .arg(invalid));
        QTimer::singleShot(0, this, [this, run] { next(run); });
      });
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
      c.recordingId = o["id"].toString();
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
  if (!fallback.fields.isEmpty())
    out << fallback;
  return out;
}
