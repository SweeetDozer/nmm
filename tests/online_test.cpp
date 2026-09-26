#include "online_dialogs.h"
#include <QTcpServer>
#include <QTcpSocket>
#include <QtTest>

namespace {
const QString recording = "11111111-1111-4111-8111-111111111111";
const QString firstRelease = "22222222-2222-4222-8222-222222222222";
const QString secondRelease = "33333333-3333-4333-8333-333333333333";
QByteArray png(Qt::GlobalColor color = Qt::blue) {
  QImage img(30, 20, QImage::Format_RGB32);
  img.fill(color);
  QByteArray data;
  QBuffer b(&data);
  b.open(QIODevice::WriteOnly);
  img.save(&b, "PNG");
  return data;
}
QJsonObject release(QString id, QString title, int number) {
  QJsonObject track{{"number", QString::number(number)},
                    {"position", number},
                    {"recording", QJsonObject{{"id", recording}}}};
  QJsonObject medium{
      {"position", 1}, {"format", "CD"}, {"tracks", QJsonArray{track}}};
  return {
      {"id", id},
      {"title", title},
      {"date", "2024-03-02"},
      {"country", "DE"},
      {"status", "Official"},
      {"artist-credit", QJsonArray{QJsonObject{{"name", "Автор альбома"}}}},
      {"release-group", QJsonObject{{"primary-type", "Album"},
                                    {"secondary-types", QJsonArray{"Live"}}}},
      {"media", QJsonArray{medium}}};
}
QByteArray releases() {
  return QJsonDocument(
             QJsonObject{
                 {"release-count", 2},
                 {"releases",
                  QJsonArray{release(firstRelease, "Первое издание", 2),
                             release(secondRelease, "Выбранное издание", 7)}}})
      .toJson();
}
Track track() {
  Track t;
  t.path = "/synthetic/Автор/Песня.mp3";
  t.duration = 200000;
  t.tags = {{"TITLE", {"Песня"}}, {"ARTIST", {"Автор"}}};
  t.cover = png(Qt::red);
  t.pictureCount = 1;
  return t;
}
Candidate candidate() {
  Candidate c;
  c.recordingId = recording;
  c.source = "MusicBrainz";
  c.fields = {{"TITLE", "Песня"}, {"ARTIST", "Автор"}};
  return c;
}
QJsonObject lyric(int id = 1, QString title = "Песня", QString artist = "Автор",
                  double length = 200) {
  return {{"id", id},
          {"trackName", title},
          {"artistName", artist},
          {"albumName", "Альбом"},
          {"duration", length},
          {"plainLyrics", "Строка теста, написанная для проверки."},
          {"syncedLyrics", "[00:01.00]Строка теста."}};
}
struct Response {
  int status = 200;
  QByteArray body = "{}";
  int delay = 0;
  qint64 announced = -1;
  QByteArray extra;
};
class Server : public QObject {
public:
  QTcpServer listener;
  QList<QUrl> requests;
  std::function<Response(QUrl)> handler;
  Server() {
    if (!listener.listen(QHostAddress::LocalHost))
      qFatal("Fixture server cannot listen");
    connect(&listener, &QTcpServer::newConnection, this, [this] {
      while (listener.hasPendingConnections()) {
        auto socket = listener.nextPendingConnection();
        auto buffer = std::make_shared<QByteArray>();
        connect(socket, &QTcpSocket::disconnected, socket,
                &QObject::deleteLater);
        connect(socket, &QTcpSocket::readyRead, this, [this, socket, buffer] {
          *buffer += socket->readAll();
          if (!buffer->contains("\r\n\r\n") ||
              socket->property("handled").toBool())
            return;
          socket->setProperty("handled", true);
          const auto line = buffer->split('\n').first().split(' ');
          QUrl url = QUrl::fromEncoded(line.value(1));
          requests << url;
          auto r = handler ? handler(url) : Response{};
          QTimer::singleShot(r.delay, socket, [socket, r] {
            if (socket->state() != QAbstractSocket::ConnectedState)
              return;
            QByteArray header =
                "HTTP/1.1 " + QByteArray::number(r.status) +
                " Fixture\r\nConnection: close\r\nContent-Length: " +
                QByteArray::number(r.announced >= 0 ? r.announced
                                                    : r.body.size()) +
                "\r\n" + r.extra + "\r\n";
            socket->write(header);
            socket->write(r.body);
            socket->disconnectFromHost();
          });
        });
      }
    });
  }
  QUrl base(QString path) const {
    return QUrl(QString("http://127.0.0.1:%1/").arg(listener.serverPort()) +
                path);
  }
  ServiceUrls urls() const {
    return {base("mb/"), base("caa/"), base("lyrics/")};
  }
};
} // namespace
class OnlineTest : public QObject {
  Q_OBJECT
private slots:
  void explicitReleaseSelection() {
    auto page = parseReleasePage(releases(), recording, 0);
    QCOMPARE(page.options.size(), 2);
    QCOMPARE(page.nextOffset, 2);
    QVERIFY(page.error.isEmpty());
    auto titleOnly = selectedProposal(track(), candidate(), {}, {});
    QVERIFY(!titleOnly.fields.contains("ALBUM"));
    QVERIFY(!titleOnly.cover);
    auto selected = selectedProposal(track(), candidate(), page.options[1], {});
    QCOMPARE(selected.fields["ALBUM"], QString("Выбранное издание"));
    QCOMPARE(selected.fields["ALBUMARTIST"], QString("Автор альбома"));
    QCOMPARE(selected.fields["TRACKNUMBER"], QString("7"));
    QCOMPARE(selected.fields["DISCNUMBER"], QString("1"));
    QCOMPARE(selected.fields["DATE"], QString("2024-03-02"));
    QVERIFY(selected.sources["ALBUM"].contains(secondRelease));
    CoverOffer other{firstRelease, "wrong", png(), {}, false};
    QVERIFY(
        !selectedProposal(track(), candidate(), page.options[1], other).cover);
    other.releaseId = secondRelease;
    QVERIFY(
        selectedProposal(track(), candidate(), page.options[1], other).cover);
    auto sparse = release(firstRelease, "Only known album", 2);
    sparse.remove("artist-credit");
    sparse.remove("date");
    sparse.remove("media");
    auto missing = parseReleasePage(
        QJsonDocument(
            QJsonObject{{"release-count", 1}, {"releases", QJsonArray{sparse}}})
            .toJson(),
        recording, 0);
    QVERIFY(!missing.options.first().fields().contains("TRACKNUMBER"));
    QVERIFY(!missing.options.first().fields().contains("DATE"));
    QVERIFY(!missing.options.first().fields().contains("ALBUMARTIST"));
  }
  void releasePagesAndPositions() {
    auto object = release(firstRelease, "Double disc", 3);
    auto media = object["media"].toArray();
    auto second = media[0].toObject();
    second["position"] = 2;
    media << second;
    object["media"] = media;
    auto page = parseReleasePage(
        QJsonDocument(QJsonObject{{"release-count", 10},
                                  {"releases", QJsonArray{object}}})
            .toJson(),
        recording, 4);
    QCOMPARE(page.options.size(), 2);
    QCOMPARE(page.options[1].discNumber, QString("2"));
    QCOMPARE(page.nextOffset, 5);
    QCOMPARE(page.total, 10);
  }
  void absentAndFailedCover_data() {
    QTest::addColumn<int>("status");
    QTest::addColumn<QByteArray>("body");
    QTest::addColumn<bool>("absent");
    QTest::newRow("not-found") << 404 << QByteArray("missing") << true;
    QTest::newRow("server-error") << 500 << QByteArray("error") << false;
    QTest::newRow("bad-image")
        << 200 << QByteArray("<html>not art</html>") << false;
  }
  void absentAndFailedCover() {
    QFETCH(int, status);
    QFETCH(QByteArray, body);
    QFETCH(bool, absent);
    Server server;
    server.handler = [=](QUrl) { return Response{status, body}; };
    QTemporaryDir cache;
    HttpClient http(nullptr, cache.path());
    OnlineServices services(http, server.urls());
    QObject context;
    bool done = false;
    CoverOffer result;
    services.cover(secondRelease, &context, [&](CoverOffer offer) {
      result = offer;
      done = true;
    });
    QTRY_VERIFY_WITH_TIMEOUT(done, 5000);
    QCOMPARE(result.absent, absent);
    if (!absent)
      QVERIFY(!result.error.isEmpty());
    QVERIFY(result.data.isEmpty());
    auto change = selectedProposal(
        track(), candidate(),
        parseReleasePage(releases(), recording, 0).options[1], result);
    QVERIFY(!change.cover);
    QCOMPARE(change.before.cover, png(Qt::red));
    QCOMPARE(server.requests.first().path(),
             "/caa/release/" + secondRelease + "/front");
  }
  void coverCachingAndBound() {
    Server server;
    server.handler = [](QUrl url) {
      if (url.path().contains(firstRelease))
        return Response{200, {}, 0, HttpClient::ImageLimit + 1};
      return Response{200, png()};
    };
    QTemporaryDir cache;
    HttpClient http(nullptr, cache.path());
    OnlineServices services(http, server.urls());
    QObject context;
    bool done = false;
    CoverOffer result;
    services.cover(secondRelease, &context, [&](CoverOffer offer) {
      result = offer;
      done = true;
    });
    QTRY_VERIFY_WITH_TIMEOUT(done, 5000);
    QCOMPARE(result.data, png());
    QCOMPARE(server.requests.size(), 1);
    done = false;
    services.cover(secondRelease, &context, [&](CoverOffer offer) {
      result = offer;
      done = true;
    });
    QTRY_VERIFY_WITH_TIMEOUT(done, 5000);
    QCOMPARE(server.requests.size(), 1);
    QCOMPARE(result.data, png());
    done = false;
    services.cover(firstRelease, &context, [&](CoverOffer offer) {
      result = offer;
      done = true;
    });
    QTRY_VERIFY_WITH_TIMEOUT(done, 5000);
    QVERIFY(result.error.contains("лимит"));
    QVERIFY(result.data.isEmpty());
  }
  void streamedLimitAndCancellation() {
    Server server;
    server.handler = [](QUrl url) {
      if (url.path() == "/slow")
        return Response{200, "{}", 500};
      return Response{200, QByteArray(HttpClient::JsonLimit + 1, ' ')};
    };
    QTemporaryDir cache;
    HttpClient http(nullptr, cache.path());
    QObject owner;
    bool done = false;
    HttpResult result;
    http.get(server.base("large"), HttpClient::Kind::Json, &owner,
             [&](HttpResult r) {
               result = r;
               done = true;
             });
    QTRY_VERIFY_WITH_TIMEOUT(done, 5000);
    QVERIFY(!result.error.isEmpty());
    done = false;
    http.get(server.base("slow"), HttpClient::Kind::Json, &owner,
             [&](HttpResult) { done = true; });
    http.cancel(&owner);
    QTest::qWait(600);
    QVERIFY(!done);
  }
  void retryAfterAndActiveCancel() {
    Server server;
    QElapsedTimer clock;
    clock.start();
    QList<qint64> starts;
    server.handler = [&](QUrl url) {
      starts << clock.elapsed();
      if (url.path() == "/limited")
        return Response{429, "{}", 0, -1, "Retry-After: 2\r\n"};
      return Response{200, "{}", url.path() == "/slow" ? 400 : 0};
    };
    QTemporaryDir cache;
    HttpClient http(nullptr, cache.path());
    QObject owner;
    bool done = false;
    http.get(server.base("limited"), HttpClient::Kind::Json, &owner,
             [&](HttpResult r) {
               QCOMPARE(r.status, 429);
               done = true;
             });
    QTRY_VERIFY_WITH_TIMEOUT(done, 3000);
    done = false;
    http.get(server.base("next"), HttpClient::Kind::Json, &owner,
             [&](HttpResult r) {
               QVERIFY(r.error.isEmpty());
               done = true;
             });
    QTRY_VERIFY_WITH_TIMEOUT(done, 5000);
    QVERIFY(starts[1] - starts[0] >= 1900);
    bool cancelledCallback = false;
    http.get(server.base("slow"), HttpClient::Kind::Json, &owner,
             [&](HttpResult) { cancelledCallback = true; });
    QTRY_COMPARE_WITH_TIMEOUT(server.requests.size(), 3, 3000);
    http.cancel(&owner);
    QTest::qWait(500);
    QVERIFY(!cancelledCallback);
  }
  void releaseDialogChoiceAndStaleCover() {
    Server server;
    server.handler = [](QUrl url) {
      if (url.path() == "/mb/release")
        return Response{200, releases()};
      return Response{
          200, png(url.path().contains(firstRelease) ? Qt::red : Qt::blue),
          100};
    };
    QTemporaryDir cache;
    HttpClient http(nullptr, cache.path());
    OnlineServices services(http, server.urls());
    RecordingDialog dialog(track(), {candidate()}, services);
    dialog.show();
    auto recordings = dialog.findChild<QListWidget *>("recordingCandidates");
    auto editions = dialog.findChild<QTableWidget *>("releaseCandidates");
    auto button = dialog.findChild<QPushButton *>("reviewRecording");
    QVERIFY(!button->isEnabled());
    recordings->setCurrentRow(0);
    QTRY_COMPARE_WITH_TIMEOUT(editions->rowCount(), 2, 5000);
    QVERIFY(editions->selectionModel()->selectedRows().isEmpty());
    QVERIFY(!dialog.proposal().fields.contains("ALBUM"));
    editions->selectRow(0);
    editions->selectRow(1);
    QCOMPARE(dialog.proposal().fields["ALBUM"], QString("Выбранное издание"));
    QVERIFY(!dialog.proposal().cover);
    QTRY_VERIFY_WITH_TIMEOUT(dialog.proposal().cover.has_value(), 5000);
    QCOMPARE(*dialog.proposal().cover, png());
    QVERIFY(dialog.proposal().sources["PICTURE"].contains(secondRelease));
    dialog.grab().save(QDir::tempPath() + "/musicorder-releases.png");
    dialog.findChild<QPushButton *>("noRelease")->click();
    QVERIFY(!dialog.proposal().cover);
    QVERIFY(!dialog.proposal().fields.contains("ALBUM"));
  }
  void lyricEvidenceAndTypes() {
    auto bytes = QJsonDocument(QJsonArray{lyric()}).toJson();
    auto result = parseLyrics(bytes, track());
    QCOMPARE(result.size(), 2);
    QVERIFY(result[0].eligible);
    QVERIFY(!result[0].synced);
    QVERIFY(!result[1].eligible);
    QVERIFY(result[1].synced);
    QVERIFY(
        !parseLyrics(
             QJsonDocument(QJsonArray{lyric(1, "Песня", "Другой")}).toJson(),
             track())[0]
             .eligible);
    QVERIFY(
        !parseLyrics(
             QJsonDocument(QJsonArray{lyric(1, "Песня", "Автор", 20)}).toJson(),
             track())[0]
             .eligible);
    QVERIFY(!parseLyrics(QJsonDocument(QJsonArray{lyric(1), lyric(2)}).toJson(),
                         track())[0]
                 .eligible);
    auto instrumental = lyric();
    instrumental["instrumental"] = true;
    QVERIFY(
        parseLyrics(QJsonDocument(QJsonArray{instrumental}).toJson(), track())
            .isEmpty());
    auto disguised = lyric();
    disguised["plainLyrics"] = "[00:02.00]This is LRC";
    QVERIFY(
        !parseLyrics(QJsonDocument(QJsonArray{disguised}).toJson(), track())[0]
             .eligible);
    auto versioned = track();
    versioned.path = "/synthetic/Песня (ремикс).mp3";
    QVERIFY(!parseLyrics(bytes, versioned)[0].eligible);
  }
  void lyricsDialogIsOptIn() {
    Server server;
    server.handler = [](QUrl) {
      return Response{200, QJsonDocument(QJsonArray{lyric()}).toJson()};
    };
    QTemporaryDir cache;
    HttpClient http(nullptr, cache.path());
    OnlineServices services(http, server.urls());
    LyricsDialog dialog(track(), services);
    dialog.show();
    QTest::qWait(30);
    QVERIFY(server.requests.isEmpty());
    QVERIFY(!dialog.proposal());
    dialog.findChild<QPushButton *>("findLyrics")->click();
    auto list = dialog.findChild<QListWidget *>("lyricCandidates");
    QTRY_COMPARE_WITH_TIMEOUT(list->count(), 2, 5000);
    QVERIFY(!dialog.proposal());
    list->setCurrentRow(1);
    QVERIFY(!dialog.proposal());
    list->setCurrentRow(0);
    QVERIFY(dialog.proposal());
    QCOMPARE(dialog.proposal()->fields.size(), 1);
    QVERIFY(dialog.proposal()->fields.contains("LYRICS"));
    QVERIFY(!dialog.proposal()->cover);
    dialog.grab().save(QDir::tempPath() + "/musicorder-lyrics.png");
    QUrlQuery query(server.requests.first());
    QCOMPARE(query.queryItemValue("track_name"), QString("Песня"));
    QCOMPARE(query.queryItemValue("artist_name"), QString("Автор"));
    QVERIFY(!query.hasQueryItem("path"));
    QVERIFY(!query.hasQueryItem("duration"));
    dialog.findChild<QLineEdit *>("lyricAlbum")->setText("Other");
    QVERIFY(!dialog.proposal());
  }
  void lyricsUnavailable_data() {
    QTest::addColumn<int>("status");
    QTest::addColumn<QByteArray>("body");
    QTest::newRow("no-results") << 200 << QByteArray("[]");
    QTest::newRow("network-error") << 500 << QByteArray("error");
  }
  void lyricsUnavailable() {
    QFETCH(int, status);
    QFETCH(QByteArray, body);
    Server server;
    server.handler = [=](QUrl) { return Response{status, body}; };
    QTemporaryDir cache;
    HttpClient http(nullptr, cache.path());
    OnlineServices services(http, server.urls());
    QObject owner;
    bool done = false;
    QList<LyricOption> result;
    services.lyrics(track(), &owner, [&](QList<LyricOption> options, QString) {
      result = options;
      done = true;
    });
    QTRY_VERIFY_WITH_TIMEOUT(done, 5000);
    QVERIFY(result.isEmpty());
  }
};
QTEST_MAIN(OnlineTest)
#include "online_test.moc"
