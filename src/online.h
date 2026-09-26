#pragma once
#include "search.h"

struct ReleaseOption {
  QString id, recordingId, album, albumArtist, date, type, country, status,
      disambiguation;
  QString trackNumber, discNumber, format;
  QMap<QString, QString> fields() const;
};
struct ReleasePage {
  QList<ReleaseOption> options;
  int nextOffset = 0, total = 0;
  QString error;
};
struct CoverOffer {
  QString releaseId, source;
  QByteArray data;
  QString error;
  bool absent = false;
};
struct LyricOption {
  QString id, title, artist, album, text, source, reason;
  double duration = 0;
  bool synced = false, eligible = false;
};
ReleasePage parseReleasePage(const QByteArray &data, const QString &recordingId,
                             int offset);
QList<LyricOption> parseLyrics(const QByteArray &data, const Track &query);
Change selectedProposal(const Track &, const Candidate &,
                        const std::optional<ReleaseOption> &,
                        const std::optional<CoverOffer> &);

struct ServiceUrls {
  QUrl musicBrainz{"https://musicbrainz.org/ws/2/"};
  QUrl coverArt{"https://coverartarchive.org/"};
  QUrl lyrics{"https://lrclib.net/api/"};
};
class OnlineServices {
public:
  explicit OnlineServices(HttpClient &client, ServiceUrls urls = {})
      : http(client), urls(std::move(urls)) {}
  void releases(const QString &recordingId, int offset, QObject *context,
                std::function<void(ReleasePage)> done);
  void cover(const QString &releaseId, QObject *context,
             std::function<void(CoverOffer)> done);
  void lyrics(const Track &query, QObject *context,
              std::function<void(QList<LyricOption>, QString)> done);
  void cancel(QObject *context) { http.cancel(context); }

private:
  HttpClient &http;
  ServiceUrls urls;
};
