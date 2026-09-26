#include "discogs.h"
namespace {
HttpOptions options(const QString &token) {
  HttpOptions o;
  o.diskCache = false;
  o.intervalMs = 2600;
  if (!token.isEmpty())
    o.headers["Authorization"] = "Discogs token=" + token.toUtf8();
  return o;
}
QString credits(const QJsonArray &artists) {
  QString result;
  for (int i = 0; i < artists.size(); ++i) {
    const auto a = artists[i].toObject();
    result += a.value("anv").toString().isEmpty() ? a.value("name").toString()
                                                  : a.value("anv").toString();
    if (i + 1 < artists.size())
      result += " " + a.value("join").toString("/") + " ";
  }
  return result.trimmed();
}
QString identity(const QJsonValue &id) {
  return id.isDouble() ? QString::number(id.toInteger()) : id.toString();
}
} // namespace
QString discogsReleaseId(const QString &input) {
  const auto text = input.trimmed();
  if (QRegularExpression("^[1-9][0-9]{0,11}$").match(text).hasMatch())
    return text;
  const auto match =
      QRegularExpression(
          "^https://(?:www\\.)?discogs\\.com/(?:[a-z]{2}/)?release/"
          "([1-9][0-9]{0,11})(?:-[^/?#]*)?(?:[?#].*)?$")
          .match(text);
  return match.hasMatch() ? match.captured(1) : QString();
}
QList<DiscogsRelease> discogsSearchResults(const QByteArray &data,
                                           QString &error) {
  auto o = QJsonDocument::fromJson(data).object();
  QList<DiscogsRelease> result;
  if (!o["results"].isArray()) {
    error = "Discogs: ответ не содержит результатов поиска";
    return result;
  }
  for (auto value : o["results"].toArray()) {
    auto r = value.toObject();
    if (r["type"].toString() != "release")
      continue;
    auto id = identity(r["id"]);
    if (discogsReleaseId(id).isEmpty())
      continue;
    result << DiscogsRelease{id, r["title"].toString() + " · " +
                                     r["country"].toString() + " · " +
                                     r["year"].toString() + " · release " + id};
  }
  return result;
}
QList<Candidate> discogsTracks(const QByteArray &data, QString &error) {
  auto o = QJsonDocument::fromJson(data).object();
  QList<Candidate> result;
  const auto id = identity(o["id"]);
  if (discogsReleaseId(id).isEmpty() || !o["tracklist"].isArray()) {
    error = "Discogs: ответ не содержит конкретного издания / треклиста";
    return result;
  }
  const auto albumArtist = credits(o["artists"].toArray());
  QString date = o["released"].toString();
  if (!QRegularExpression(
           "^\\d{4}(?:-(?:0[1-9]|1[0-2])(?:-(?:0[1-9]|[12][0-9]|3[01]))?)?$")
           .match(date)
           .hasMatch())
    date =
        o["year"].toInt() > 0 ? QString::number(o["year"].toInt()) : QString();
  std::function<void(QJsonArray)> parse = [&](QJsonArray list) {
    for (auto value : list) {
      auto tr = value.toObject();
      if (tr["type_"].toString() == "heading")
        continue;
      if (tr["sub_tracks"].isArray()) {
        parse(tr["sub_tracks"].toArray());
        continue;
      }
      if (tr["title"].toString().isEmpty())
        continue;
      Candidate c;
      c.fields["TITLE"] = tr["title"].toString();
      const auto performer = tr["artists"].isArray()
                                 ? credits(tr["artists"].toArray())
                                 : albumArtist;
      if (!performer.isEmpty() && normalized(performer) != "various")
        c.fields["ARTIST"] = performer;
      if (!o["title"].toString().isEmpty())
        c.fields["ALBUM"] = o["title"].toString();
      if (!albumArtist.isEmpty())
        c.fields["ALBUMARTIST"] = albumArtist;
      if (!date.isEmpty())
        c.fields["DATE"] = date;
      if (!tr["position"].toString().isEmpty())
        c.fields["TRACKNUMBER"] = tr["position"].toString();
      c.source =
          "Data provided by Discogs · https://www.discogs.com/release/" + id;
      c.reason =
          "Поля из конкретного издания Discogs: " + c.fields.keys().join(", ") +
          ". Длительность в каталоге: " +
          tr["duration"].toString("не указана") +
          ". Соответствие вашему аудио не доказано; выберите трек вручную. "
          "Связь с MusicBrainz не предполагается.";
      result << c;
    }
  };
  parse(o["tracklist"].toArray());
  return result;
}
void DiscogsService::search(
    QString title, QString artist, QString token, QObject *context,
    std::function<void(QList<DiscogsRelease>, QString)> done) {
  if (token.isEmpty() || token.contains('\n') || token.contains('\r')) {
    QTimer::singleShot(0, context, [done] {
      done({}, "Поиск API Discogs требует личный токен. Без токена используйте "
               "поиск в браузере и загрузку издания по ID.");
    });
    return;
  }
  auto url = base.resolved(QUrl("database/search"));
  QUrlQuery q;
  q.addQueryItem("type", "release");
  q.addQueryItem("track", title);
  if (!artist.isEmpty())
    q.addQueryItem("artist", artist);
  q.addQueryItem("per_page", "20");
  q.addQueryItem("page", "1");
  url.setQuery(q);
  http.get(
      url, HttpClient::Kind::Json, context,
      [done, token](HttpResult reply) {
        QString error = reply.error;
        auto list = error.isEmpty() ? discogsSearchResults(reply.data, error)
                                    : QList<DiscogsRelease>();
        error.replace(token, "[токен скрыт]");
        done(list, error);
      },
      options(token));
}
void DiscogsService::release(
    QString id, QString token, QObject *context,
    std::function<void(QList<Candidate>, QString)> done) {
  if (discogsReleaseId(id) != id || token.contains('\n') ||
      token.contains('\r')) {
    QTimer::singleShot(0, context,
                       [done] { done({}, "Некорректный ID издания / токен"); });
    return;
  }
  http.get(
      base.resolved(QUrl("releases/" + id)), HttpClient::Kind::Json, context,
      [done, id, token](HttpResult reply) {
        QString error = reply.error;
        QList<Candidate> list;
        if (error.isEmpty() &&
            identity(QJsonDocument::fromJson(reply.data).object()["id"]) != id)
          error = "Discogs вернул другое издание. Ответ отклонён.";
        if (error.isEmpty())
          list = discogsTracks(reply.data, error);
        if (!token.isEmpty())
          error.replace(token, "[токен скрыт]");
        done(list, error);
      },
      options(token));
}
DiscogsDialog::DiscogsDialog(const Track &t, DiscogsService &s, QString value,
                             QWidget *parent)
    : QDialog(parent), track(t), service(s), token(std::move(value)) {
  setObjectName("discogsDialog");
  setWindowTitle("Discogs · выберите издание и трек");
  resize(1000, 850);
  auto layout = new QVBoxLayout(this);
  auto info =
      new QLabel("API-поиск требует личный токен. Без него можно открыть поиск "
                 "сайта и загрузить выбранное издание по ID или ссылке. Аудио "
                 "не передаётся. Discogs — самостоятельный каталог; совпадение "
                 "не подтверждено MusicBrainz.");
  info->setWordWrap(true);
  layout->addWidget(info);
  const auto local = localSuggestion(t);
  title = new QLineEdit(meaningfulSearchText(t.value("TITLE"))
                            ? cleanSearchText(t.value("TITLE"))
                            : local.value("TITLE"));
  artist = new QLineEdit(meaningfulSearchText(t.value("ARTIST"))
                             ? cleanSearchText(t.value("ARTIST"))
                             : local.value("ARTIST"));
  auto form = new QFormLayout;
  form->addRow("Название трека", title);
  form->addRow("Исполнитель", artist);
  layout->addLayout(form);
  auto actions = new QHBoxLayout;
  auto find = new QPushButton("Искать через API");
  find->setObjectName("discogsSearch");
  find->setEnabled(!token.isEmpty());
  if (token.isEmpty())
    find->setToolTip(
        "Добавьте личный токен в настройках; режим по ID доступен ниже");
  auto browser = new QPushButton("Поиск на сайте Discogs");
  actions->addWidget(find);
  actions->addWidget(browser);
  layout->addLayout(actions);
  releases = new QListWidget;
  releases->setObjectName("discogsReleases");
  layout->addWidget(releases, 1);
  auto direct = new QHBoxLayout;
  id = new QLineEdit;
  id->setObjectName("discogsId");
  id->setPlaceholderText("Числовой ID или https://www.discogs.com/release/…");
  auto loadButton = new QPushButton("Загрузить издание");
  loadButton->setObjectName("loadDiscogsRelease");
  direct->addWidget(id, 1);
  direct->addWidget(loadButton);
  layout->addLayout(direct);
  tracks = new QListWidget;
  tracks->setObjectName("discogsTracks");
  tracks->setWordWrap(true);
  layout->addWidget(tracks, 1);
  source = new QLabel;
  source->setWordWrap(true);
  source->setOpenExternalLinks(true);
  layout->addWidget(source);
  log = new QPlainTextEdit;
  log->setReadOnly(true);
  log->setMaximumHeight(140);
  layout->addWidget(log);
  auto buttons = new QDialogButtonBox;
  apply =
      buttons->addButton("Трек в предпросмотр", QDialogButtonBox::AcceptRole);
  apply->setObjectName("reviewDiscogs");
  apply->setEnabled(false);
  buttons->addButton(QDialogButtonBox::Cancel);
  layout->addWidget(buttons);
  connect(find, &QPushButton::clicked, this, &DiscogsDialog::search);
  connect(browser, &QPushButton::clicked, this, [this] {
    QUrl url("https://www.discogs.com/search/");
    QUrlQuery q;
    q.addQueryItem("q", artist->text() + " " + title->text());
    q.addQueryItem("type", "release");
    url.setQuery(q);
    QDesktopServices::openUrl(url);
    message("Открыт поиск сайта Discogs. Перенесите ID выбранного издания в "
            "поле ниже.");
  });
  connect(loadButton, &QPushButton::clicked, this,
          [this] { load(discogsReleaseId(id->text())); });
  connect(releases, &QListWidget::currentRowChanged, this, [this](int row) {
    if (row >= 0 && row < releaseOptions.size()) {
      id->setText(releaseOptions[row].id);
      load(releaseOptions[row].id);
    }
  });
  connect(tracks, &QListWidget::currentRowChanged, this, [this](int row) {
    apply->setEnabled(row >= 0 && row < options.size());
    if (row >= 0 && row < options.size())
      message(options[row].reason);
  });
  connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
}
DiscogsDialog::~DiscogsDialog() { reset(); }
void DiscogsDialog::reset() {
  if (request) {
    service.cancel(request);
    delete request;
    request = nullptr;
  }
  options.clear();
  tracks->clear();
  apply->setEnabled(false);
  source->clear();
}
void DiscogsDialog::message(const QString &s) {
  events << s;
  log->appendPlainText(s);
}
void DiscogsDialog::search() {
  reset();
  releases->clear();
  releaseOptions.clear();
  if (!meaningfulSearchText(title->text())) {
    message("Discogs: нет осмысленного названия для поиска. Можно загрузить "
            "издание по ID.");
    return;
  }
  message("Discogs · GET /database/search · track=" + title->text() +
          " · artist=" + artist->text() + " · до 20 изданий. Токен скрыт.");
  request = new QObject(this);
  service.search(
      title->text(), artist->text(), token, request,
      [this](QList<DiscogsRelease> list, QString error) {
        if (!error.isEmpty()) {
          message("Discogs · ОШИБКА, не «не найдено»: " + error);
          return;
        }
        releaseOptions = list;
        message(QString("Discogs · получено %1 изданий. Каждое требует ручного "
                        "выбора; не более первой страницы.")
                    .arg(list.size()));
        for (auto &r : list) {
          auto item = new QListWidgetItem(releases);
          auto label =
              new QLabel(r.label.toHtmlEscaped() +
                         "<br><a href=\"https://www.discogs.com/release/" +
                         r.id + "\">Data provided by Discogs</a>");
          label->setOpenExternalLinks(true);
          item->setSizeHint(QSize(400, 50));
          releases->setItemWidget(item, label);
        }
      });
}
void DiscogsDialog::load(const QString &releaseId) {
  reset();
  if (releaseId.isEmpty()) {
    message(
        "Нужен ID конкретного release, не master и не произвольная ссылка.");
    return;
  }
  message("Discogs · GET /releases/" + releaseId + " · " +
          (token.isEmpty() ? "без токена" : "с токеном (скрыт)"));
  request = new QObject(this);
  service.release(
      releaseId, token, request,
      [this, releaseId](QList<Candidate> list, QString error) {
        if (!error.isEmpty()) {
          message("Discogs · ОШИБКА загрузки: " + error);
          return;
        }
        options = list;
        message(QString("Discogs · получено %1 треков. Ничего не выбрано "
                        "автоматически.")
                    .arg(list.size()));
        source->setText("<a href=\"https://www.discogs.com/release/" +
                        releaseId + "\">Data provided by Discogs · издание " +
                        releaseId + "</a>");
        for (auto &c : options)
          tracks->addItem(c.fields.value("TRACKNUMBER") + " · " +
                          c.fields.value("ARTIST") + " — " +
                          c.fields.value("TITLE") +
                          "\nАльбом: " + c.fields.value("ALBUM") + " · " +
                          c.fields.value("DATE") + "\n" + c.source);
      });
}
std::optional<Change> DiscogsDialog::proposal() const {
  const int row = tracks->currentRow();
  if (row < 0 || row >= options.size())
    return {};
  const auto &c = options[row];
  Change change{track, c.fields, {}, {}};
  for (auto it = c.fields.begin(); it != c.fields.end(); ++it)
    change.sources[it.key()] = c.source;
  return change;
}
