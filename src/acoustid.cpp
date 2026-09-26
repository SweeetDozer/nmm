#include "acoustid.h"
#include <cmath>
Fingerprint parseFingerprint(const QByteArray &data) {
  Fingerprint result;
  QJsonParseError error;
  auto o = QJsonDocument::fromJson(data, &error).object();
  const double duration = o["duration"].toDouble();
  result.value = o["fingerprint"].toString();
  if (error.error != QJsonParseError::NoError || !std::isfinite(duration) ||
      duration < 1 || duration > 86400 || result.value.size() < 8 ||
      result.value.size() > 200000 ||
      !QRegularExpression("^[A-Za-z0-9_=\\-]+$")
           .match(result.value)
           .hasMatch()) {
    result.error = "fpcalc вернул некорректный отпечаток / длительность";
    result.value.clear();
    return result;
  }
  result.duration = qRound(duration);
  return result;
}
QList<Candidate> acoustIdCandidates(const QByteArray &data, QString &error,
                                    QStringList &diagnostics) {
  auto doc = QJsonDocument::fromJson(data);
  auto root = doc.object();
  QList<Candidate> result;
  if (root["status"].toString() != "ok") {
    error = "AcoustID: " + root["error"].toObject()["message"].toString(
                               "Некорректный ответ сервиса");
    return result;
  }
  if (!root["results"].isArray()) {
    error = "AcoustID: отсутствует массив результатов";
    return result;
  }
  const auto matches = root["results"].toArray();
  diagnostics
      << QString("AcoustID · совпадений отпечатка: %1").arg(matches.size());
  for (auto match : matches) {
    auto m = match.toObject();
    const auto recordings = m["recordings"].toArray();
    diagnostics << QString("AcoustID %1 · score %2 · связанных записей %3")
                       .arg(m["id"].toString())
                       .arg(m["score"].toDouble(), 0, 'f', 3)
                       .arg(recordings.size());
    if (recordings.isEmpty())
      diagnostics << "Совпадение не связано с записью MusicBrainz: текстовых "
                     "тегов для предложения нет.";
    for (auto recording : recordings) {
      auto o = recording.toObject();
      Candidate c;
      c.recordingId = o["id"].toString();
      if (c.recordingId.isEmpty()) {
        diagnostics << "Отклонена повреждённая связь: нет MBID.";
        continue;
      }
      const auto title = o["title"].toString();
      if (!title.isEmpty())
        c.fields["TITLE"] = title;
      QStringList artists;
      for (auto artist : o["artists"].toArray()) {
        const auto name = artist.toObject()["name"].toString();
        if (!name.isEmpty())
          artists << name;
      }
      if (!artists.isEmpty())
        c.fields["ARTIST"] = artists.join(" / ");
      c.source = "AcoustID · " + m["id"].toString() + " → MusicBrainz · " +
                 c.recordingId;
      c.reason =
          QString(
              "Совпадение отпечатка: score %1 (не вероятность). Связанная "
              "запись: %2. Метаданные MusicBrainz возвращены AcoustID; "
              "длительность %3 с. Предложены только полученные поля: %4. Нужна "
              "ручная проверка версии; запись не разрешена автоматически.")
              .arg(m["score"].toDouble(), 0, 'f', 3)
              .arg(c.recordingId)
              .arg(o["duration"].toDouble())
              .arg(c.fields.keys().join(", "));
      c.reliable = false;
      result << c;
    }
  }
  return result;
}
AcoustId::AcoustId(HttpClient &client, QObject *parent, QUrl url)
    : MetadataSource(parent), http(client), endpoint(std::move(url)) {
  timeout.setSingleShot(true);
  timeout.setInterval(120000);
  connect(&timeout, &QTimer::timeout, this, [this] {
    if (running) {
      processError = "Превышено время вычисления отпечатка (120 с)";
      process.kill();
    }
  });
  connect(&process, &QProcess::readyReadStandardOutput, this, [this] {
    output += process.readAllStandardOutput();
    if (output.size() > 1024 * 1024) {
      processError = "Слишком большой ответ fpcalc";
      process.kill();
    }
  });
  connect(&process, &QProcess::readyReadStandardError, this,
          [this] { process.readAllStandardError(); });
  connect(&process, &QProcess::errorOccurred, this,
          [this](QProcess::ProcessError error) {
            if (running && error == QProcess::FailedToStart)
              fail("Не удалось запустить fpcalc. Установите chromaprint-tools "
                   "или задайте путь в настройках.");
          });
  connect(&process, &QProcess::finished, this, &AcoustId::calculated);
}
AcoustId::~AcoustId() { cancel(); }
void AcoustId::configure(QString value, QString path) {
  key = value.trimmed();
  executable = path.trimmed();
}
void AcoustId::cancel() {
  ++generation;
  running = false;
  timeout.stop();
  http.cancel(this);
  if (process.state() != QProcess::NotRunning) {
    process.kill();
    process.waitForFinished(1000);
  }
}
void AcoustId::fail(const QString &message) {
  running = false;
  timeout.stop();
  QString error = message;
  if (!key.isEmpty())
    error.replace(key, "[ключ скрыт]");
  emit diagnostic(error);
  emit ready(current, {}, error);
}
void AcoustId::lookup(const Track &track) {
  cancel();
  current = track;
  const auto run = generation;
  if (key.isEmpty()) {
    QTimer::singleShot(0, this, [this, run] {
      if (run == generation)
        fail("AcoustID: задайте собственный application/client key в "
             "настройках. Запрос не отправлен.");
    });
    return;
  }
  QString path = executable.isEmpty() ? QStandardPaths::findExecutable("fpcalc")
                                      : executable;
  if (path.isEmpty()) {
    QTimer::singleShot(0, this, [this, run] {
      if (run == generation)
        fail("Chromaprint: fpcalc не найден. Установите chromaprint-tools или "
             "задайте путь в настройках.");
    });
    return;
  }
  emit diagnostic("Chromaprint · локальный отпечаток первых 120 с: " +
                  QFileInfo(track.path).fileName() +
                  ". Аудиофайл не загружается.");
  output.clear();
  processError.clear();
  running = true;
  timeout.start();
  process.start(path, {"-json", "-length", "120",
                       QFileInfo(track.path).absoluteFilePath()});
}
void AcoustId::calculated(int code, QProcess::ExitStatus status) {
  if (!running)
    return;
  running = false;
  timeout.stop();
  output += process.readAllStandardOutput();
  if (!processError.isEmpty()) {
    fail(processError);
    return;
  }
  // fpcalc can report decoder warnings; only a successful process with valid
  // JSON is accepted.
  if (status != QProcess::NormalExit || code != 0) {
    fail("fpcalc не смог декодировать файл (код " + QString::number(code) +
         "). Теги не изменены.");
    return;
  }
  const auto fingerprint = parseFingerprint(output);
  output.clear();
  if (!fingerprint.error.isEmpty()) {
    fail(fingerprint.error);
    return;
  }
  emit diagnostic(
      QString(
          "AcoustID · POST /v2/lookup · duration=%1 · meta=recordings · размер "
          "отпечатка %2 символов. Ключ и отпечаток в журнал не выводятся.")
          .arg(fingerprint.duration)
          .arg(fingerprint.value.size()));
  HttpOptions options;
  options.diskCache = false;
  auto add = [&](QByteArray name, QString value) {
    if (!options.form.isEmpty())
      options.form += '&';
    options.form += name + '=' + QUrl::toPercentEncoding(value);
  };
  add("client", key);
  add("duration", QString::number(fingerprint.duration));
  add("fingerprint", fingerprint.value);
  add("meta", "recordings");
  add("format", "json");
  const auto run = generation;
  http.get(
      endpoint, HttpClient::Kind::Json, this,
      [this, run](HttpResult reply) {
        if (run != generation)
          return;
        if (!reply.error.isEmpty()) {
          fail("AcoustID · сетевая ошибка, не отсутствие совпадений: " +
               reply.error);
          return;
        }
        QString error;
        QStringList logs;
        auto candidates = acoustIdCandidates(reply.data, error, logs);
        for (auto &line : logs)
          emit diagnostic(line);
        if (!error.isEmpty()) {
          fail(error);
          return;
        }
        if (candidates.isEmpty())
          emit diagnostic("AcoustID: сервер не вернул связанных записей. Сеть "
                          "ответила успешно.");
        emit ready(current, candidates, {});
      },
      options);
}
