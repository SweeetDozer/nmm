#pragma once
#include "online.h"
#include <QtWidgets>
void showFullImage(QWidget *parent, const QByteArray &data,
                   const QString &source);

class RecordingDialog : public QDialog {
public:
  RecordingDialog(const Track &, const QList<Candidate> &, OnlineServices &,
                  QWidget *parent = nullptr, QString diagnostics = {});
  ~RecordingDialog() override;
  Change proposal() const;
  bool stopRequested() const { return stop; }

private:
  Track track;
  QList<Candidate> candidates;
  OnlineServices &services;
  QListWidget *recordings;
  QTableWidget *editions;
  QLabel *state, *art, *artSource;
  QPushButton *more, *apply, *full;
  QList<ReleaseOption> options;
  std::optional<ReleaseOption> selectedRelease;
  std::optional<CoverOffer> selectedCover;
  QObject *releaseContext = nullptr, *coverContext = nullptr;
  int offset = 0;
  bool stop = false;
  void clearContext(QObject *&context);
  void recordingChanged(int row);
  void loadPage();
  void releaseChanged();
};
class LyricsDialog : public QDialog {
public:
  LyricsDialog(const Track &, OnlineServices &, QWidget *parent = nullptr);
  ~LyricsDialog() override;
  std::optional<Change> proposal() const;

private:
  Track track;
  OnlineServices &services;
  QObject *context = nullptr;
  QLineEdit *title, *artist, *album;
  QListWidget *list;
  QPlainTextEdit *text;
  QLabel *state;
  QPushButton *search, *apply;
  QList<LyricOption> options;
  void startSearch();
  void invalidate();
};
