#pragma once
#include "acoustid.h"
#include "core.h"
#include "discogs.h"
#include "online_dialogs.h"
#include "search.h"
#include <QAudioOutput>
#include <QMediaPlayer>
#include <QtConcurrent>
#include <QtWidgets>
class TrackModel : public QAbstractTableModel {
public:
  QList<Track> tracks;
  using QAbstractTableModel::QAbstractTableModel;
  int rowCount(const QModelIndex &p = {}) const override {
    return p.isValid() ? 0 : tracks.size();
  }
  int columnCount(const QModelIndex &p = {}) const override {
    return p.isValid() ? 0 : 10;
  }
  QVariant data(const QModelIndex &i, int role) const override;
  QVariant headerData(int section, Qt::Orientation o, int role) const override;
  void append(const QList<Track> &batch);
  void update(const Track &track);
  void clear();
};
class TrackFilter : public QSortFilterProxyModel {
public:
  int mode = 0;
  using QSortFilterProxyModel::QSortFilterProxyModel;
  void setMode(int value) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
    beginFilterChange();
    mode = value;
    endFilterChange(Direction::Rows);
#else
    mode = value;
    invalidateFilter();
#endif
  }
  bool filterAcceptsRow(int row, const QModelIndex &parent) const override;
};
class Window : public QMainWindow {
  Q_OBJECT
public:
  Window();
  ~Window() override;
  void openCollection(const QString &path);
signals:
  void collectionLoaded(int count);

private:
  TrackModel *model;
  TrackFilter *proxy;
  QTableView *table;
  QLabel *status;
  QProgressBar *progress;
  QPlainTextEdit *log;
  QMediaPlayer *player;
  QAudioOutput *audio;
  QLabel *nowPlaying;
  QString root, storage;
  std::atomic_bool cancelled{false};
  QFuture<void> job;
  bool busy = false;
  MusicBrainz *source;
  AcoustId *acoustid;
  MetadataSource *activeSource = nullptr;
  std::unique_ptr<DiscogsService> discogs;
  QString acoustidKey, discogsToken, fpcalcPath;
  QStringList searchJournal, trackJournal;
  void searchSettings();
  void searchDiagnostic(const QString &message);
  void showSearchJournal();
  void findDiscogs();
  HttpClient *network;
  std::unique_ptr<OnlineServices> online;
  QAction *lyricsAction = nullptr;
  void findLyrics();
  QList<QAction *> taskActions;
  QPushButton *cancelButton;
  QList<Change> searchChanges;
  int searchDone = 0, searchTotal = 0;
  QList<Track> searchQueue;
  void setBusy(bool value);
  void scan();
  void edit();
  QList<Track> selected() const;
  void play(const QString &path);
  void preview(QList<Change> changes);
  void write(QList<Change> changes);
  void duplicates();
  void search(bool fingerprint = false);
  void nextSearch();
  void chooseCandidates(const Track &, const QList<Candidate> &,
                        const QString &);
  void message(const QString &s);
  void closeEvent(QCloseEvent *e) override;
};
