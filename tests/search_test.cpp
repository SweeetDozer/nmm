#include "search.h"
#include <QtTest>
class SearchTest : public QObject {
  Q_OBJECT
private:
  Track track() {
    Track t;
    t.path = "/music/Автор/Песня.mp3";
    t.duration = 200000;
    t.tags = {{"TITLE", {"Песня"}}, {"ARTIST", {"Автор"}}};
    return t;
  }
  QByteArray response(QString title = "Песня", QString artist = "Автор",
                      int length = 200000, int copies = 1,
                      QString disambiguation = {}) {
    QJsonObject recording{
        {"id", "example-id"},
        {"title", title},
        {"length", length},
        {"disambiguation", disambiguation},
        {"artist-credit", QJsonArray{QJsonObject{{"name", artist}}}},
        {"releases", QJsonArray{QJsonObject{{"title", "Не выбирать"}}}}};
    QJsonArray recordings;
    while (copies--)
      recordings << recording;
    return QJsonDocument(QJsonObject{{"recordings", recordings}}).toJson();
  }
private slots:
  void strictEvidence() {
    auto t = track();
    auto matches = musicBrainzCandidates(t, response());
    QVERIFY(matches.first().reliable);
    QVERIFY(!matches.first().fields.contains("ALBUM"));
    QVERIFY(!matches.first().fields.contains("DATE"));
    QVERIFY(!musicBrainzCandidates(t, response("Песня", "Другой автор"))
                 .first()
                 .reliable);
    QVERIFY(!musicBrainzCandidates(t, response("Песня", "Автор", 100000))
                 .first()
                 .reliable);
    QVERIFY(!musicBrainzCandidates(t, response("Песня", "Автор", 0))
                 .first()
                 .reliable);
    QVERIFY(
        !musicBrainzCandidates(t, response("Песня", "Автор", 200000, 1, "live"))
             .first()
             .reliable);
  }
  void ambiguity() {
    auto matches =
        musicBrainzCandidates(track(), response("Песня", "Автор", 200000, 2));
    QVERIFY(!matches[0].reliable);
    QVERIFY(!matches[1].reliable);
  }
  void fallback() {
    auto t = track();
    t.tags.clear();
    auto result = musicBrainzCandidates(t, {});
    QCOMPARE(result.size(), 1);
    QCOMPARE(result.first().fields.value("TITLE"), QString("Песня"));
    QCOMPARE(result.first().fields.value("ARTIST"), QString("Автор"));
    QVERIFY(!result.first().reliable);
    QCOMPARE(result.first().fields.size(), 2);
  }
  void filenameEvidence() {
    auto t = track();
    t.tags["TITLE"] = {"001"};
    t.tags["ARTIST"] = {"999"};
    QVERIFY(musicBrainzCandidates(t, response()).first().reliable);
    t = track();
    t.path = "/music/Автор/Песня (ремикс).mp3";
    QVERIFY(!musicBrainzCandidates(t, response()).first().reliable);
  }
  void liveService() {
    if (!qEnvironmentVariableIsSet("MUSICORDER_LIVE_TEST"))
      QSKIP("Optional network check; set MUSICORDER_LIVE_TEST=1");
    QStandardPaths::setTestModeEnabled(true);
    MusicBrainz source;
    source.setContact("https://github.com/SweeetDozer/nmm");
    Track t;
    t.path = "/music/The Beatles/Hey Jude.mp3";
    t.tags = {{"TITLE", {"Hey Jude"}}, {"ARTIST", {"The Beatles"}}};
    bool done = false;
    QString error;
    QList<Candidate> found;
    connect(&source, &MetadataSource::ready, this,
            [&](const Track &, const QList<Candidate> &results,
                const QString &message) {
              found = results;
              error = message;
              done = true;
            });
    source.lookup(t);
    QTRY_VERIFY_WITH_TIMEOUT(done, 30000);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY(found.size() > 1);
    QVERIFY(found.first().source.startsWith("MusicBrainz"));
  }
  void versionPreserved() {
    QCOMPARE(cleanTitle("Песня remix [abcdefghijk].mp3"),
             QString("Песня remix"));
    QVERIFY(cleanTitle("Песня (Live).mp3").contains("Live"));
  }
};
QTEST_GUILESS_MAIN(SearchTest)
#include "search_test.moc"
