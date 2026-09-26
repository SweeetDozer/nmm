#include "acoustid.h"
#include "discogs.h"
#include <QTcpServer>
#include <QTcpSocket>
#include <QtTest>
namespace {
struct Request {
  QUrl url;
  QByteArray method, headers, body;
};
struct Reply {
  int status = 200;
  QByteArray data = "{}";
  int delay = 0;
};
class Server : public QObject {
public:
  QTcpServer tcp;
  QList<Request> received;
  std::function<Reply(const Request &)> handler;
  Server() {
    if (!tcp.listen(QHostAddress::LocalHost))
      qFatal("Cannot start fixture server");
    connect(&tcp, &QTcpServer::newConnection, this, [this] {
      while (tcp.hasPendingConnections()) {
        auto socket = tcp.nextPendingConnection();
        auto bytes = std::make_shared<QByteArray>();
        connect(socket, &QTcpSocket::disconnected, socket,
                &QObject::deleteLater);
        connect(socket, &QTcpSocket::readyRead, this, [this, socket, bytes] {
          *bytes += socket->readAll();
          int end = bytes->indexOf("\r\n\r\n");
          if (end < 0 || socket->property("handled").toBool())
            return;
          auto headers = bytes->left(end);
          auto length =
              QRegularExpression("Content-Length: (\\d+)",
                                 QRegularExpression::CaseInsensitiveOption)
                  .match(QString::fromLatin1(headers));
          int size = length.hasMatch() ? length.captured(1).toInt() : 0;
          if (bytes->size() < end + 4 + size)
            return;
          socket->setProperty("handled", true);
          auto first = headers.split('\n').first().split(' ');
          Request request{QUrl::fromEncoded(first.value(1)), first.value(0),
                          headers, bytes->mid(end + 4, size)};
          received << request;
          auto reply = handler ? handler(request) : Reply{};
          QTimer::singleShot(reply.delay, socket, [socket, reply] {
            if (socket->state() != QAbstractSocket::ConnectedState)
              return;
            socket->write("HTTP/1.1 " + QByteArray::number(reply.status) +
                          " Fixture\r\nContent-Length: " +
                          QByteArray::number(reply.data.size()) +
                          "\r\nConnection: close\r\n\r\n" + reply.data);
            socket->disconnectFromHost();
          });
        });
      }
    });
  }
  QUrl url(QString path = {}) const {
    return QUrl(QString("http://127.0.0.1:%1/").arg(tcp.serverPort()) + path);
  }
};
Track track() {
  Track t;
  t.path = "/synthetic/Редкий автор/01. Редкий автор - Песня (Official Video) "
           "[abcdefghijk].mp3";
  t.duration = 12000;
  t.tags = {{"TITLE", {"999"}}, {"ARTIST", {"Unknown Artist"}}};
  return t;
}
QByteArray mbResults() {
  QJsonArray records;
  for (QString id : {"11111111-1111-4111-8111-111111111111",
                     "22222222-2222-4222-8222-222222222222"})
    records << QJsonObject{
        {"id", id},
        {"title", "Песня (live)"},
        {"length", 100000},
        {"artist-credit", QJsonArray{QJsonObject{{"name", "Другой автор"}}}}};
  records << QJsonObject{{"id", "malformed"}};
  return QJsonDocument(QJsonObject{{"recordings", records}, {"count", 3}})
      .toJson();
}
QByteArray acoustResults() {
  QJsonArray recordings;
  for (QString id : {"11111111-1111-4111-8111-111111111111",
                     "22222222-2222-4222-8222-222222222222"})
    recordings << QJsonObject{
        {"id", id},
        {"title", "Песня"},
        {"duration", 12},
        {"artists", QJsonArray{QJsonObject{{"name", "Редкий автор"}}}}};
  return QJsonDocument(
             QJsonObject{{"status", "ok"},
                         {"results",
                          QJsonArray{QJsonObject{{"id", "fixture-match"},
                                                 {"score", 0.87},
                                                 {"recordings", recordings}}}}})
      .toJson();
}
QByteArray release() {
  return QJsonDocument(
             QJsonObject{
                 {"id", 42},
                 {"title", "Конкретный альбом"},
                 {"year", 2020},
                 {"artists",
                  QJsonArray{QJsonObject{{"name", "Исполнитель альбома"}}}},
                 {"tracklist",
                  QJsonArray{QJsonObject{{"title", "Не первый трек"},
                                         {"position", "1"},
                                         {"type_", "track"}},
                             QJsonObject{{"title", "Выбранная песня"},
                                         {"position", "2"},
                                         {"type_", "track"},
                                         {"artists",
                                          QJsonArray{QJsonObject{
                                              {"name", "Другой автор"}}}}}}}})
      .toJson();
}
} // namespace
class CatalogTest : public QObject {
  Q_OBJECT
private slots:
  void queryPlanning() {
    auto t = track();
    auto unchanged = t;
    auto plan = musicBrainzQueries(t);
    QCOMPARE(plan.size(), 2);
    QVERIFY(plan[0].query.contains("Редкий автор"));
    QVERIFY(!plan[1].query.contains("artist:"));
    QVERIFY(!plan[0].query.contains("999"));
    QVERIFY(!plan[0].query.contains("Official"));
    QCOMPARE(t.path, unchanged.path);
    QCOMPARE(t.tags, unchanged.tags);
    t.tags = {{"TITLE", {"Название из тегов"}}, {"ARTIST", {"Автор тегов"}}};
    QCOMPARE(musicBrainzQueries(t).size(), 4);
    t.tags.clear();
    t.path = "/Music/unknown/12345.mp3";
    QVERIFY(musicBrainzQueries(t).isEmpty());
    t.tags = {{"TITLE", {"Настоящая песня"}}, {"ARTIST", {"Автор"}}};
    QVERIFY(!musicBrainzQueries(t).isEmpty());
    QCOMPARE(localSuggestion(track()).value("TITLE"), QString("Песня"));
    QVERIFY(cleanTitle("Автор - Песня (remix).mp3").contains("remix"));
  }
  void fallbackKeepsUnconfirmed() {
    Server server;
    server.handler = [](const Request &r) {
      auto query = QUrlQuery(r.url).queryItemValue("query");
      return Reply{200, query.contains("artist:")
                            ? QByteArray("{\"recordings\":[],\"count\":0}")
                            : mbResults()};
    };
    QTemporaryDir cache;
    HttpClient http(nullptr, cache.path());
    MusicBrainz source(nullptr, &http, server.url("mb"));
    QStringList logs;
    QList<Candidate> matches;
    QString error;
    bool done = false;
    connect(&source, &MetadataSource::diagnostic,
            [&](QString line) { logs << line; });
    connect(&source, &MetadataSource::ready,
            [&](const Track &, const QList<Candidate> &list, QString e) {
              matches = list;
              error = e;
              done = true;
            });
    source.lookup(track());
    QTRY_VERIFY_WITH_TIMEOUT(done, 6000);
    QVERIFY(error.isEmpty());
    QCOMPARE(server.received.size(), 2);
    QCOMPARE(matches.size(), 3);
    QVERIFY(!matches[0].reliable);
    QVERIFY(!matches[1].reliable);
    QVERIFY(logs.join("\n").contains("ПОКАЗАН"));
    QVERIFY(logs.join("\n").contains("повреждённый"));
    QVERIFY(logs.join("\n").contains("получено 0"));
  }
  void emptyVsError_data() {
    QTest::addColumn<int>("status");
    QTest::addColumn<QByteArray>("body");
    QTest::addColumn<bool>("failure");
    QTest::newRow("empty") << 200 << QByteArray("{\"recordings\":[]}") << false;
    QTest::newRow("network") << 500 << QByteArray("offline") << true;
    QTest::newRow("bad-schema") << 200 << QByteArray("{}") << true;
  }
  void emptyVsError() {
    QFETCH(int, status);
    QFETCH(QByteArray, body);
    QFETCH(bool, failure);
    Server server;
    server.handler = [=](const Request &) { return Reply{status, body}; };
    QTemporaryDir cache;
    HttpClient http(nullptr, cache.path());
    MusicBrainz source(nullptr, &http, server.url("mb"));
    QString error;
    QStringList logs;
    bool done = false;
    connect(&source, &MetadataSource::diagnostic,
            [&](QString s) { logs << s; });
    connect(&source, &MetadataSource::ready,
            [&](const Track &, const QList<Candidate> &, QString e) {
              error = e;
              done = true;
            });
    source.lookup(track());
    QTRY_VERIFY_WITH_TIMEOUT(done, 6000);
    QCOMPARE(!error.isEmpty(), failure);
    if (failure) {
      QCOMPARE(server.received.size(), 1);
      QVERIFY(logs.join("\n").contains("ОШИБКА"));
    }
  }
  void noMeaningfulQuery() {
    Server server;
    QTemporaryDir cache;
    HttpClient http(nullptr, cache.path());
    MusicBrainz source(nullptr, &http, server.url("mb"));
    auto t = track();
    t.path = "/Music/Unknown/1234.mp3";
    bool done = false;
    QStringList logs;
    connect(&source, &MetadataSource::diagnostic,
            [&](QString s) { logs << s; });
    connect(&source, &MetadataSource::ready,
            [&](const Track &, const QList<Candidate> &list, QString e) {
              QVERIFY(list.isEmpty());
              QVERIFY(e.isEmpty());
              done = true;
            });
    source.lookup(t);
    QTRY_VERIFY(done);
    QVERIFY(server.received.isEmpty());
    QVERIFY(logs.join("\n").contains("не отправлены"));
  }
  void multipleFingerprintLinks() {
    QString error;
    QStringList logs;
    auto candidates = acoustIdCandidates(acoustResults(), error, logs);
    QVERIFY(error.isEmpty());
    QCOMPARE(candidates.size(), 2);
    QVERIFY(candidates[0].recordingId != candidates[1].recordingId);
    QVERIFY(candidates[0].source.startsWith("AcoustID"));
    QVERIFY(candidates[0].reason.contains("0.870"));
    QVERIFY(!candidates[0].reliable);
    QCOMPARE(candidates[0].fields.size(), 2);
    auto bad = acoustIdCandidates(
        "{\"status\":\"error\",\"error\":{\"message\":\"invalid key\"}}", error,
        logs);
    QVERIFY(bad.isEmpty());
    QVERIFY(!error.isEmpty());
    QVERIFY(!parseFingerprint("{}").error.isEmpty());
  }
  void missingKeyIsNotNoMatch() {
    Server server;
    QTemporaryDir cache;
    HttpClient http(nullptr, cache.path());
    AcoustId source(http, nullptr, server.url("lookup"));
    bool done = false;
    QString error;
    connect(&source, &MetadataSource::ready,
            [&](const Track &, const QList<Candidate> &list, QString e) {
              QVERIFY(list.isEmpty());
              error = e;
              done = true;
            });
    source.lookup(track());
    QTRY_VERIFY(done);
    QVERIFY(error.contains("client key"));
    QVERIFY(server.received.isEmpty());
  }
  void processAndPrivatePost() {
#ifndef Q_OS_UNIX
    QSKIP("Shell fixture is Unix-only; real fpcalc test is portable");
#else
    QTemporaryDir files;
    QFile fake(files.path() + "/fpcalc");
    QVERIFY(fake.open(QIODevice::WriteOnly));
    fake.write(
        "#!/bin/sh\nprintf '%s' "
        "'{\"duration\":12,\"fingerprint\":\"TEST_FINGERPRINT_123\"}'\n");
    fake.close();
    QVERIFY(fake.setPermissions(QFileDevice::ReadOwner |
                                QFileDevice::WriteOwner |
                                QFileDevice::ExeOwner));
    Server server;
    server.handler = [](const Request &) {
      return Reply{200, acoustResults()};
    };
    QTemporaryDir cache;
    HttpClient http(nullptr, cache.path());
    AcoustId source(http, nullptr, server.url("lookup"));
    source.configure("fixture-client-key", fake.fileName());
    bool done = false;
    QString error;
    QStringList logs;
    connect(&source, &MetadataSource::diagnostic,
            [&](QString s) { logs << s; });
    connect(&source, &MetadataSource::ready,
            [&](const Track &, const QList<Candidate> &list, QString e) {
              error = e;
              done = true;
              QCOMPARE(list.size(), 2);
            });
    source.lookup(track());
    QTRY_VERIFY_WITH_TIMEOUT(done, 5000);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(server.received.size(), 1);
    const auto r = server.received[0];
    QCOMPARE(r.method, QByteArray("POST"));
    QVERIFY(r.url.query().isEmpty());
    QUrlQuery form(QString::fromUtf8(r.body));
    QCOMPARE(form.queryItemValue("client"), QString("fixture-client-key"));
    QVERIFY(form.hasQueryItem("fingerprint"));
    QCOMPARE(form.queryItemValue("duration"), QString("12"));
    QVERIFY(!r.body.contains(track().path.toUtf8()));
    QVERIFY(!logs.join("\n").contains("fixture-client-key"));
    QVERIFY(!logs.join("\n").contains("TEST_FINGERPRINT_123"));
    QVERIFY(QDir(cache.path()).entryList(QDir::Files).isEmpty());
#endif
  }
  void realFingerprintOnTemporaryAudio() {
    const auto fp = qEnvironmentVariable(
        "MUSICORDER_FPCALC", QStandardPaths::findExecutable("fpcalc"));
    if (fp.isEmpty())
      QSKIP("Install chromaprint-tools or set MUSICORDER_FPCALC for local "
            "audio integration");
    QTemporaryDir files;
    auto path = files.path() + "/123456.flac";
    QProcess ffmpeg;
    ffmpeg.start("ffmpeg",
                 {"-v", "error", "-f", "lavfi", "-i",
                  "sine=frequency=440:duration=12", "-c:a", "flac", path});
    QVERIFY(ffmpeg.waitForFinished());
    QCOMPARE(ffmpeg.exitCode(), 0);
    const auto before = fileHash(path);
    Server server;
    server.handler = [](const Request &) {
      return Reply{200, acoustResults()};
    };
    QTemporaryDir cache;
    HttpClient http(nullptr, cache.path());
    AcoustId source(http, nullptr, server.url("lookup"));
    source.configure("fixture-client-key", fp);
    bool done = false;
    QString error;
    connect(&source, &MetadataSource::ready,
            [&](const Track &, const QList<Candidate> &list, QString e) {
              error = e;
              done = true;
              QCOMPARE(list.size(), 2);
            });
    source.lookup(readTrack(path));
    QTRY_VERIFY_WITH_TIMEOUT(done, 15000);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(fileHash(path), before);
    QCOMPARE(server.received.size(), 1);
    QVERIFY(!server.received[0].body.contains("fLaC"));
  }
  void discogsWithoutTokenAndTrackChoice() {
    Server server;
    server.handler = [](const Request &) { return Reply{200, release()}; };
    QTemporaryDir cache;
    HttpClient http(nullptr, cache.path());
    DiscogsService service(http, server.url());
    DiscogsDialog dialog(track(), service, {});
    dialog.show();
    auto search = dialog.findChild<QPushButton *>("discogsSearch");
    QVERIFY(!search->isEnabled());
    auto id = dialog.findChild<QLineEdit *>("discogsId");
    id->setText("https://www.discogs.com/release/42-Test");
    dialog.findChild<QPushButton *>("loadDiscogsRelease")->click();
    auto list = dialog.findChild<QListWidget *>("discogsTracks");
    QTRY_COMPARE_WITH_TIMEOUT(list->count(), 2, 5000);
    QVERIFY(!dialog.proposal());
    list->setCurrentRow(1);
    auto change = dialog.proposal();
    QVERIFY(change);
    QCOMPARE(change->fields["TITLE"], QString("Выбранная песня"));
    QCOMPARE(change->fields["ARTIST"], QString("Другой автор"));
    QCOMPARE(change->fields["TRACKNUMBER"], QString("2"));
    QVERIFY(change->sources["TITLE"].contains("Discogs"));
    QVERIFY(!change->cover);
    QVERIFY(!server.received[0].headers.contains("Authorization"));
    QVERIFY(QDir(cache.path()).entryList(QDir::Files).isEmpty());
    dialog.grab().save(QDir::tempPath() + "/musicorder-discogs.png");
  }
  void discogsTokenSearchAndErrors() {
    Server server;
    server.handler = [](const Request &r) {
      return r.url.path().contains("search")
                 ? Reply{200, "{\"results\":[{\"id\":42,\"type\":\"release\","
                              "\"title\":\"Artist - Album\"}]}"}
                 : Reply{200, "{\"id\":43,\"tracklist\":[]}"};
    };
    QTemporaryDir cache;
    HttpClient http(nullptr, cache.path());
    DiscogsService service(http, server.url());
    QObject owner;
    bool done = false;
    QString error;
    service.search("Title", "Artist", {}, &owner,
                   [&](QList<DiscogsRelease> list, QString e) {
                     QVERIFY(list.isEmpty());
                     error = e;
                     done = true;
                   });
    QTRY_VERIFY(done);
    QVERIFY(!error.isEmpty());
    QVERIFY(server.received.isEmpty());
    done = false;
    service.search("Title", "Artist", "fixture-token", &owner,
                   [&](QList<DiscogsRelease> list, QString e) {
                     QCOMPARE(list.size(), 1);
                     error = e;
                     done = true;
                   });
    QTRY_VERIFY_WITH_TIMEOUT(done, 5000);
    QVERIFY(error.isEmpty());
    QVERIFY(server.received[0].headers.contains(
        "Authorization: Discogs token=fixture-token"));
    QVERIFY(!server.received[0].url.toString().contains("fixture-token"));
    done = false;
    service.release("42", {}, &owner, [&](QList<Candidate> list, QString e) {
      QVERIFY(list.isEmpty());
      error = e;
      done = true;
    });
    QTRY_VERIFY_WITH_TIMEOUT(done, 5000);
    QVERIFY(error.contains("другое издание"));
    QVERIFY(discogsReleaseId("https://evil.test/release/42").isEmpty());
  }
};
QTEST_MAIN(CatalogTest)
#include "catalog_test.moc"
