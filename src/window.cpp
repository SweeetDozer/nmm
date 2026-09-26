#include "window.h"
#include <algorithm>
namespace {
QString duration(int ms) {
  return QString("%1:%2")
      .arg(ms / 60000)
      .arg(ms / 1000 % 60, 2, 10, QChar('0'));
}
QString fieldName(const QString &f) {
  static QMap<QString, QString> names = {
      {"TITLE", "Название"},      {"ARTIST", "Исполнитель"},
      {"ALBUM", "Альбом"},        {"ALBUMARTIST", "Исполнитель альбома"},
      {"TRACKNUMBER", "№ трека"}, {"DATE", "Год / дата"},
      {"GENRE", "Жанр"},          {"COMMENT", "Комментарий"},
      {"LYRICS", "Текст песни"},  {"DISCNUMBER", "№ диска"},
      {"PICTURE", "Обложка"}};
  return names.value(f, f);
}
QTableWidgetItem *cell(const QString &s) {
  auto i = new QTableWidgetItem(s);
  i->setFlags(i->flags() & ~Qt::ItemIsEditable);
  return i;
}
} // namespace
QVariant TrackModel::data(const QModelIndex &i, int role) const {
  if (!i.isValid())
    return {};
  const auto &t = tracks[i.row()];
  if (role == Qt::ForegroundRole && !t.error.isEmpty())
    return QColor("#e47272");
  if (role == Qt::ToolTipRole)
    return t.path + (t.error.isEmpty() ? "" : "\n" + t.error);
  if (role == Qt::UserRole) {
    switch (i.column()) {
    case 4:
      return t.duration;
    case 6:
      return t.bitrate;
    case 7:
      return t.sampleRate;
    default:
      return data(i, Qt::DisplayRole);
    }
  }
  if (role != Qt::DisplayRole)
    return {};
  switch (i.column()) {
  case 0:
    return t.value("TITLE");
  case 1:
    return t.value("ARTIST");
  case 2:
    return t.value("ALBUM");
  case 3:
    return QFileInfo(t.path).fileName();
  case 4:
    return duration(t.duration);
  case 5:
    return t.format;
  case 6:
    return t.bitrate;
  case 7:
    return t.sampleRate;
  case 8:
    return t.error.isEmpty()
               ? (t.value("TITLE").isEmpty() || t.value("ARTIST").isEmpty()
                      ? "Неполные теги"
                      : "Прочитан")
               : t.error;
  case 9:
    return t.path;
  }
  return {};
}
QVariant TrackModel::headerData(int n, Qt::Orientation o, int role) const {
  if (role != Qt::DisplayRole)
    return {};
  if (o == Qt::Vertical)
    return n + 1;
  return QStringList{"Название", "Исполнитель", "Альбом", "Файл",      "Время",
                     "Формат",   "кбит/с",      "Гц",     "Состояние", "Путь"}
      .value(n);
}
void TrackModel::append(const QList<Track> &b) {
  if (b.isEmpty())
    return;
  beginInsertRows({}, tracks.size(), tracks.size() + b.size() - 1);
  tracks.append(b);
  endInsertRows();
}
void TrackModel::clear() {
  beginResetModel();
  tracks.clear();
  endResetModel();
}
void TrackModel::update(const Track &t) {
  for (int i = 0; i < tracks.size(); ++i)
    if (tracks[i].path == t.path) {
      tracks[i] = t;
      tracks[i].cover.clear();
      tracks[i].coverLoaded = false;
      emit dataChanged(index(i, 0), index(i, 9));
      break;
    }
}
bool TrackFilter::filterAcceptsRow(int row, const QModelIndex &parent) const {
  auto m = static_cast<TrackModel *>(sourceModel());
  const auto &t = m->tracks[row];
  if (mode == 1 && !t.value("TITLE").isEmpty() && !t.value("ARTIST").isEmpty())
    return false;
  if (mode == 2 && t.error.isEmpty())
    return false;
  if (mode >= 3 &&
      t.format != QStringList{"MP3", "FLAC", "OGG"}.value(mode - 3))
    return false;
  return QSortFilterProxyModel::filterAcceptsRow(row, parent);
}
Window::Window() {
  setWindowTitle("MusicOrder — порядок в музыке");
  resize(1280, 820);
  storage = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
  QDir().mkpath(storage);
  auto center = new QWidget;
  auto layout = new QVBoxLayout(center);
  layout->setContentsMargins(22, 18, 22, 12);
  layout->setSpacing(12);
  setCentralWidget(center);
  auto title = new QLabel("MusicOrder");
  title->setStyleSheet("font-size:28px;font-weight:700");
  layout->addWidget(title);
  auto subtitle = new QLabel("Локальная коллекция · теги · проверка повторов");
  layout->addWidget(subtitle);
  auto toolbar = new QToolBar;
  toolbar->setToolButtonStyle(Qt::ToolButtonTextOnly);
  layout->addWidget(toolbar);
  auto add = [&](QString name, auto fn) {
    auto a = toolbar->addAction(name);
    connect(a, &QAction::triggered, this, fn);
    taskActions << a;
  };
  add("Открыть папку", [this] {
    QString p =
        QFileDialog::getExistingDirectory(this, "Корень коллекции", root);
    if (!p.isEmpty())
      openCollection(p);
  });
  add("Обновить", [this] {
    if (!root.isEmpty())
      scan();
  });
  add("Редактор", [this] { edit(); });
  add("Из имён файлов", [this] {
    QList<Change> list;
    for (auto &t : selected())
      if (t.error.isEmpty())
        list << Change{t, localSuggestion(t), {}};
    preview(list);
  });
  add("MusicBrainz", [this] { search(); });
  add("Распознать AcoustID", [this] { search(true); });
  add("Discogs", [this] { findDiscogs(); });
  add("Настройки поиска", [this] { searchSettings(); });
  auto journal = toolbar->addAction("Журнал поиска");
  connect(journal, &QAction::triggered, this, &Window::showSearchJournal);
  add("Текст песни", [this] { findLyrics(); });
  lyricsAction = taskActions.last();
  lyricsAction->setEnabled(false);
  add("Повторы", [this] { duplicates(); });
  toolbar->addSeparator();
  auto backups = toolbar->addAction("Копии и восстановление");
  connect(backups, &QAction::triggered, this,
          [this] { QDesktopServices::openUrl(QUrl::fromLocalFile(storage)); });
  auto filters = new QHBoxLayout;
  auto searchBox = new QLineEdit;
  searchBox->setPlaceholderText("Поиск по тегам, имени или пути…");
  filters->addWidget(searchBox, 1);
  auto filter = new QComboBox;
  filter->addItems(
      {"Все файлы", "Неполные теги", "Ошибки", "MP3", "FLAC", "OGG"});
  filters->addWidget(filter);
  layout->addLayout(filters);
  model = new TrackModel(this);
  proxy = new TrackFilter(this);
  proxy->setSourceModel(model);
  proxy->setFilterKeyColumn(-1);
  proxy->setFilterCaseSensitivity(Qt::CaseInsensitive);
  proxy->setSortRole(Qt::UserRole);
  connect(searchBox, &QLineEdit::textChanged, proxy,
          &QSortFilterProxyModel::setFilterFixedString);
  connect(filter, &QComboBox::currentIndexChanged, proxy,
          &TrackFilter::setMode);
  table = new QTableView;
  table->setModel(proxy);
  table->setSortingEnabled(true);
  table->setSelectionBehavior(QAbstractItemView::SelectRows);
  table->setSelectionMode(QAbstractItemView::ExtendedSelection);
  table->setAlternatingRowColors(true);
  table->verticalHeader()->hide();
  table->setWordWrap(false);
  table->horizontalHeader()->setStretchLastSection(true);
  table->setColumnWidth(0, 200);
  table->setColumnWidth(1, 180);
  table->setColumnWidth(3, 230);
  layout->addWidget(table, 1);
  connect(table, &QTableView::doubleClicked, this, [this] {
    if (!busy)
      edit();
  });
  auto playback = new QHBoxLayout;
  auto playButton = new QPushButton("▶ Слушать");
  auto pause = new QPushButton("Пауза");
  auto stop = new QPushButton("Стоп");
  nowPlaying = new QLabel("Выберите трек для прослушивания");
  playback->addWidget(playButton);
  playback->addWidget(pause);
  playback->addWidget(stop);
  playback->addWidget(nowPlaying, 1);
  layout->addLayout(playback);
  player = new QMediaPlayer(this);
  audio = new QAudioOutput(this);
  player->setAudioOutput(audio);
  audio->setVolume(0.7);
  connect(playButton, &QPushButton::clicked, this, [this] {
    auto s = selected();
    if (!s.isEmpty())
      play(s.first().path);
  });
  connect(pause, &QPushButton::clicked, player, &QMediaPlayer::pause);
  connect(stop, &QPushButton::clicked, player, &QMediaPlayer::stop);
  connect(player, &QMediaPlayer::errorOccurred, this,
          [this](QMediaPlayer::Error, const QString &s) {
            message("Воспроизведение: " + s);
          });
  auto bottom = new QHBoxLayout;
  status = new QLabel("Откройте папку с музыкой");
  progress = new QProgressBar;
  progress->setMaximumWidth(250);
  progress->setRange(0, 1);
  progress->setValue(0);
  cancelButton = new QPushButton("Отмена");
  cancelButton->setEnabled(false);
  bottom->addWidget(status, 1);
  bottom->addWidget(progress);
  bottom->addWidget(cancelButton);
  layout->addLayout(bottom);
  log = new QPlainTextEdit;
  log->setReadOnly(true);
  log->setMaximumBlockCount(10000);
  log->setMaximumHeight(105);
  log->setPlaceholderText("Журнал операций. Резервные копии: " + storage);
  layout->addWidget(log);
  network = new HttpClient(this);
  online = std::make_unique<OnlineServices>(*network);
  source = new MusicBrainz(this, network);
  acoustid = new AcoustId(*network, this);
  discogs = std::make_unique<DiscogsService>(*network);
  QSettings preferences;
  acoustidKey = preferences.value("acoustid/clientKey").toString();
  discogsToken = preferences.value("discogs/token").toString();
  fpcalcPath = preferences.value("chromaprint/executable").toString();
  acoustid->configure(acoustidKey, fpcalcPath);
  for (auto provider : QList<MetadataSource *>{source, acoustid}) {
    connect(provider, &MetadataSource::diagnostic, this,
            &Window::searchDiagnostic);
    connect(provider, &MetadataSource::ready, this, &Window::chooseCandidates);
  }
  connect(
      table->selectionModel(), &QItemSelectionModel::selectionChanged, this,
      [this] { lyricsAction->setEnabled(!busy && selected().size() == 1); });

  connect(cancelButton, &QPushButton::clicked, this, [this] {
    cancelled = true;
    if (activeSource)
      activeSource->cancel();
    if (!searchQueue.isEmpty() || searchTotal) {
      searchQueue.clear();
      searchTotal = 0;
      setBusy(false);
      preview(searchChanges);
    }
  });
  setStyleSheet(
      "QMainWindow,QDialog{background:#161a21;color:#e5e9f0} "
      "QWidget{font-size:13px} "
      "QTableView,QListWidget,QPlainTextEdit,QLineEdit,QComboBox{background:#"
      "202630;color:#"
      "e5e9f0;border:1px solid #374151;border-radius:5px;padding:5px} "
      "QTableView{alternate-background-color:#252c37;selection-background-"
      "color:#345b70} "
      "QHeaderView::section{background:#252c37;color:#c3cdd9;padding:8px;"
      "border:0} QLabel{color:#dce4ed} QPushButton,QToolButton{padding:8px "
      "12px;background:#2d3948;color:#edf3f8;border:1px solid "
      "#465569;border-radius:5px} "
      "QPushButton:hover,QToolButton:hover{background:#3c5264} "
      "QPushButton:disabled,QToolButton:disabled{color:#7a8593} "
      "QProgressBar{border:1px solid #374151;text-align:center;color:#e5e9f0} "
      "QProgressBar::chunk{background:#4b998d}");
}
Window::~Window() {
  cancelled = true;
  delete acoustid;
  acoustid = nullptr;
  activeSource = nullptr;
  source->cancel();
  job.waitForFinished();
}
void Window::closeEvent(QCloseEvent *e) {
  if (busy) {
    QMessageBox::information(
        this, "Операция выполняется",
        "Отмените текущую операцию и дождитесь её завершения перед выходом.");
    e->ignore();
  } else
    e->accept();
}
void Window::message(const QString &s) {
  log->appendPlainText(QTime::currentTime().toString("HH:mm:ss") + "  " + s);
}
void Window::setBusy(bool b) {
  busy = b;
  for (auto a : taskActions)
    a->setEnabled(!b);
  cancelButton->setEnabled(b);
  if (lyricsAction)
    lyricsAction->setEnabled(!b && selected().size() == 1);
  if (!b) {
    progress->setRange(0, 1);
    progress->setValue(1);
  }
}
QList<Track> Window::selected() const {
  QList<Track> out;
  for (auto i : table->selectionModel()->selectedRows())
    out << model->tracks[proxy->mapToSource(i).row()];
  return out;
}
void Window::play(const QString &p) {
  player->setSource(QUrl::fromLocalFile(p));
  player->play();
  nowPlaying->setText(QFileInfo(p).fileName());
}
void Window::openCollection(const QString &path) {
  if (busy || !QFileInfo(path).isDir())
    return;
  root = QFileInfo(path).absoluteFilePath();
  scan();
}
void Window::scan() {
  setBusy(true);
  cancelled = false;
  model->clear();
  status->setText("Поиск аудиофайлов…");
  progress->setRange(0, 0);
  QString folder = root;
  job = QtConcurrent::run([this, folder] {
    QStringList paths;
    QDirIterator it(folder, QDir::Files | QDir::NoSymLinks,
                    QDirIterator::Subdirectories);
    while (it.hasNext() && !cancelled) {
      QString p = it.next();
      if (QStringList{"mp3", "flac", "ogg"}.contains(
              QFileInfo(p).suffix().toLower()) &&
          !p.startsWith(storage + "/"))
        paths << p;
    }
    int count = 0;
    QList<Track> batch;
    for (const auto &p : paths) {
      if (cancelled)
        break;
      batch << readTrack(p, false);
      ++count;
      if (batch.size() >= 50 || count == paths.size()) {
        auto copy = batch;
        batch.clear();
        QMetaObject::invokeMethod(this, [this, copy, count,
                                         total = paths.size()] {
          model->append(copy);
          progress->setRange(0, total);
          progress->setValue(count);
          status->setText(QString("Прочитано %1 / %2").arg(count).arg(total));
          for (auto &t : copy)
            if (!t.error.isEmpty())
              message(t.path + ": " + t.error);
        });
      }
    }
    QMetaObject::invokeMethod(this, [this, count] {
      status->setText(QString("%1 файлов · %2%3")
                          .arg(count)
                          .arg(root, cancelled ? " · отменено" : ""));
      setBusy(false);
      emit collectionLoaded(count);
    });
  });
}
void Window::edit() {
  auto items = selected();
  if (items.isEmpty())
    return;
  Track t = readTrack(items.first().path);
  if (!t.error.isEmpty()) {
    message(t.error);
    return;
  }
  QDialog dialog(this);
  dialog.setWindowTitle("Редактор · " + QFileInfo(t.path).fileName());
  dialog.resize(780, 760);
  auto v = new QVBoxLayout(&dialog);
  auto path = new QLabel(t.path);
  path->setWordWrap(true);
  path->setTextInteractionFlags(Qt::TextSelectableByMouse);
  v->addWidget(path);
  auto tabs = new QTabWidget;
  v->addWidget(tabs, 1);
  auto fields = new QWidget;
  auto form = new QFormLayout(fields);
  QMap<QString, QLineEdit *> edits;
  QPlainTextEdit *lyrics = nullptr;
  for (auto f : editableFields()) {
    if (f == "LYRICS") {
      lyrics = new QPlainTextEdit(t.value(f));
      lyrics->setPlaceholderText("Обычный текст. Поиск для выбранного трека — "
                                 "кнопка «Текст песни» в главном окне");
      form->addRow(fieldName(f), lyrics);
    } else {
      auto e = new QLineEdit(t.value(f));
      edits[f] = e;
      form->addRow(fieldName(f), e);
    }
  }
  tabs->addTab(fields, "Основные поля");
  auto extra = new QPlainTextEdit;
  extra->setReadOnly(true);
  QStringList lines;
  for (auto it = t.tags.begin(); it != t.tags.end(); ++it)
    lines << it.key() + " = " + it.value().join(" | ");
  lines << "" << "Сложные поля (сохраняются): " + t.complexKeys.join(", ")
        << "Нераспознанные поля TagLib (сохраняются без редактирования): " +
               t.unsupported.join(", ");
  extra->setPlainText(lines.join("\n"));
  tabs->addTab(extra, "Все текстовые поля");
  auto art = new QWidget;
  auto artLayout = new QVBoxLayout(art);
  auto cover = new QLabel;
  cover->setAlignment(Qt::AlignCenter);
  auto showCover = [cover](const QByteArray &b) {
    QPixmap pix;
    pix.loadFromData(b);
    if (pix.isNull()) {
      cover->setPixmap({});
      cover->setText("Обложки нет или она не декодируется");
    } else
      cover->setPixmap(
          pix.scaled(420, 420, Qt::KeepAspectRatio, Qt::SmoothTransformation));
  };
  showCover(t.cover);
  artLayout->addWidget(cover, 1);
  auto hint = new QLabel(QString("Обложек: %1. Показана первая. Замена или "
                                 "удаление применяется ко всем обложкам файла.")
                             .arg(t.pictureCount));
  hint->setWordWrap(true);
  artLayout->addWidget(hint);
  std::optional<QByteArray> changedCover;
  auto replace = new QPushButton("Загрузить JPEG / PNG");
  auto remove = new QPushButton("Удалить обложки");
  artLayout->addWidget(replace);
  artLayout->addWidget(remove);
  tabs->addTab(art, "Обложка");
  connect(replace, &QPushButton::clicked, &dialog, [&] {
    QString p = QFileDialog::getOpenFileName(
        &dialog, "Обложка", {}, "Изображения (*.jpg *.jpeg *.png)");
    if (p.isEmpty())
      return;
    QFile f(p);
    if (!f.open(QIODevice::ReadOnly) || f.size() > 20 * 1024 * 1024) {
      QMessageBox::warning(&dialog, "Обложка",
                           "Файл недоступен или больше 20 МБ");
      return;
    }
    auto b = f.readAll();
    if ((!b.startsWith("\x89PNG") && !b.startsWith("\xff\xd8")) ||
        QImage::fromData(b).isNull()) {
      QMessageBox::warning(&dialog, "Обложка", "Нужен корректный JPEG или PNG");
      return;
    }
    changedCover = b;
    showCover(b);
  });
  connect(remove, &QPushButton::clicked, &dialog, [&] {
    changedCover = QByteArray();
    showCover({});
  });
  auto listen = new QPushButton("▶ Прослушать");
  v->addWidget(listen);
  connect(listen, &QPushButton::clicked, &dialog, [this, t] { play(t.path); });
  auto buttons =
      new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
  v->addWidget(buttons);
  connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
  if (dialog.exec() != QDialog::Accepted)
    return;
  Change c;
  c.before = t;
  c.cover = changedCover;
  for (auto it = edits.begin(); it != edits.end(); ++it)
    if (it.value()->text() != t.value(it.key()))
      c.fields[it.key()] = it.value()->text();
  if (lyrics->toPlainText() != t.value("LYRICS"))
    c.fields["LYRICS"] = lyrics->toPlainText();
  preview({c});
}
void Window::preview(QList<Change> changes) {
  QDialog d(this);
  d.setWindowTitle("Предпросмотр изменений — выберите поля");
  d.resize(1100, 650);
  auto v = new QVBoxLayout(&d);
  auto info = new QLabel(
      "Отметьте нужные поля. Существующие значения требуют отдельного выбора. "
      "Файлы и звук не переименовываются и не перекодируются.");
  info->setWordWrap(true);
  v->addWidget(info);
  auto grid = new QTableWidget(0, 6);
  grid->setObjectName("changesPreview");
  grid->setHorizontalHeaderLabels(
      {"Применить", "Файл", "Поле", "Сейчас", "Предложение", "Источник"});
  grid->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
  grid->horizontalHeader()->setStretchLastSection(true);
  for (int column : {1, 3, 4}) {
    grid->horizontalHeader()->setSectionResizeMode(column,
                                                   QHeaderView::Interactive);
    grid->setColumnWidth(column, 210);
  }
  grid->setIconSize(QSize(64, 64));
  v->addWidget(grid, 1);
  struct Row {
    int change;
    QString field;
  };
  QList<Row> rows;
  for (int i = 0; i < changes.size(); ++i) {
    const auto &c = changes[i];
    auto add = [&](QString f, QString before, QString after) {
      int r = grid->rowCount();
      grid->insertRow(r);
      auto check = cell("");
      check->setFlags(check->flags() | Qt::ItemIsUserCheckable);
      check->setCheckState(Qt::Unchecked);
      grid->setItem(r, 0, check);
      grid->setItem(r, 1, cell(c.before.path));
      grid->setItem(r, 2, cell(fieldName(f)));
      grid->setItem(r, 3, cell(before));
      grid->setItem(r, 4, cell(after));
      grid->setItem(
          r, 5, cell(c.sources.value(f, "Ручной выбор / локальная подсказка")));
      if (f == "PICTURE") {
        QPixmap oldArt, newArt;
        oldArt.loadFromData(c.before.cover);
        newArt.loadFromData(*c.cover);
        grid->item(r, 3)->setIcon(QIcon(oldArt));
        grid->item(r, 4)->setIcon(QIcon(newArt));
        grid->setRowHeight(r, 80);
      }
      rows << Row{i, f};
    };
    for (auto it = c.fields.begin(); it != c.fields.end(); ++it)
      if (c.before.value(it.key()) != it.value())
        add(it.key(), c.before.value(it.key()), it.value());
    if (c.cover && *c.cover != c.before.cover)
      add("PICTURE",
          c.before.pictureCount > 0
              ? QString("Существующих обложек: %1 · %2 байт (первая)")
                    .arg(c.before.pictureCount)
                    .arg(c.before.cover.size())
              : "Обложки нет",
          (c.cover->isEmpty()
               ? QString("Удаление всех обложек")
               : (c.before.pictureCount > 0
                      ? QString("ЗАМЕНА ВСЕХ существующих обложек")
                      : QString("Добавление обложки")) +
                     QString(" · %1 байт").arg(c.cover->size())));
  }
  connect(grid, &QTableWidget::cellDoubleClicked, &d, [&](int row, int column) {
    if (row < 0 || row >= rows.size())
      return;
    const auto entry = rows[row];
    const auto &change = changes[entry.change];
    if (entry.field == "PICTURE")
      showFullImage(&d, column == 3 ? change.before.cover : *change.cover,
                    column == 3 ? "Существующая обложка"
                                : change.sources.value("PICTURE",
                                                       "Предлагаемая обложка"));
    else if (entry.field == "LYRICS") {
      QDialog full(&d);
      full.resize(800, 650);
      full.setWindowTitle("Полный текст");
      auto layout = new QVBoxLayout(&full);
      auto text =
          new QPlainTextEdit(column == 3 ? change.before.value("LYRICS")
                                         : change.fields.value("LYRICS"));
      text->setReadOnly(true);
      layout->addWidget(text);
      auto b = new QDialogButtonBox(QDialogButtonBox::Close);
      connect(b, &QDialogButtonBox::rejected, &full, &QDialog::reject);
      layout->addWidget(b);
      full.exec();
    }
  });
  if (rows.isEmpty()) {
    message("Нет новых значений для предпросмотра. Выберите файлы в таблице.");
    return;
  }
  auto choose = new QHBoxLayout;
  auto empty = new QPushButton("Выбрать только пустые поля");
  auto all = new QPushButton("Выбрать все показанные изменения");
  auto none = new QPushButton("Снять выбор");
  choose->addWidget(empty);
  choose->addWidget(all);
  choose->addWidget(none);
  v->addLayout(choose);
  connect(empty, &QPushButton::clicked, &d, [&] {
    for (int r = 0; r < rows.size(); ++r)
      grid->item(r, 0)->setCheckState(rows[r].field != "PICTURE" &&
                                              grid->item(r, 3)->text().isEmpty()
                                          ? Qt::Checked
                                          : Qt::Unchecked);
  });
  connect(all, &QPushButton::clicked, &d, [&] {
    for (int r = 0; r < rows.size(); ++r)
      grid->item(r, 0)->setCheckState(Qt::Checked);
  });
  connect(none, &QPushButton::clicked, &d, [&] {
    for (int r = 0; r < rows.size(); ++r)
      grid->item(r, 0)->setCheckState(Qt::Unchecked);
  });
  auto buttons =
      new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
  buttons->button(QDialogButtonBox::Ok)->setText("К итоговому списку");
  v->addWidget(buttons);
  connect(buttons, &QDialogButtonBox::accepted, &d, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, &d, &QDialog::reject);
  if (d.exec() != QDialog::Accepted)
    return;
  QMap<int, Change> chosen;
  QStringList summary;
  for (int r = 0; r < rows.size(); ++r)
    if (grid->item(r, 0)->checkState() == Qt::Checked) {
      auto row = rows[r];
      if (!chosen.contains(row.change))
        chosen[row.change] = Change{changes[row.change].before, {}, {}};
      chosen[row.change].sources[row.field] =
          changes[row.change].sources.value(row.field);
      if (row.field == "PICTURE")
        chosen[row.change].cover = changes[row.change].cover;
      else
        chosen[row.change].fields[row.field] =
            changes[row.change].fields[row.field];
      summary << grid->item(r, 1)->text() + "\n" + grid->item(r, 2)->text() +
                     ": " + grid->item(r, 3)->text() + " → " +
                     grid->item(r, 4)->text() +
                     "\nИсточник: " + grid->item(r, 5)->text();
    }
  if (chosen.isEmpty())
    return;
  QDialog confirm(this);
  confirm.setWindowTitle("Подтверждение записи");
  confirm.resize(850, 550);
  auto cv = new QVBoxLayout(&confirm);
  auto label = new QLabel(
      QString("Будет изменено файлов: %1. Полные копии: %2/backups\nПри ошибке "
              "оригинал остаётся в резервной копии; см. restore.json.")
          .arg(chosen.size())
          .arg(storage));
  label->setWordWrap(true);
  cv->addWidget(label);
  auto text = new QPlainTextEdit(summary.join("\n\n"));
  text->setReadOnly(true);
  cv->addWidget(text);
  auto cb =
      new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
  cv->addWidget(cb);
  connect(cb, &QDialogButtonBox::accepted, &confirm, &QDialog::accept);
  connect(cb, &QDialogButtonBox::rejected, &confirm, &QDialog::reject);
  if (confirm.exec() == QDialog::Accepted)
    write(chosen.values());
}
void Window::write(QList<Change> changes) {
  player->stop();
  setBusy(true);
  cancelled = false;
  progress->setRange(0, changes.size());
  progress->setValue(0);
  job = QtConcurrent::run([this, changes] {
    int count = 0, failed = 0;
    for (auto &c : changes) {
      if (cancelled)
        break;
      auto r = writeTrack(c, storage + "/backups");
      ++count;
      if (!r.error.isEmpty())
        ++failed;
      QMetaObject::invokeMethod(this, [this, r, count] {
        if (!r.after.path.isEmpty())
          model->update(r.after);
        message(r.path + ": " +
                (r.error.isEmpty() ? "Сохранено и проверено" : r.error) +
                "\nКопия: " + r.backup);
        progress->setValue(count);
      });
    }
    QMetaObject::invokeMethod(this, [this, count, failed] {
      status->setText(QString("Обработано %1 · ошибок %2%3")
                          .arg(count)
                          .arg(failed)
                          .arg(cancelled ? " · отменено" : ""));
      setBusy(false);
    });
  });
}
void Window::search(bool fingerprint) {
  searchQueue = selected();
  if (searchQueue.isEmpty()) {
    message("Выберите файлы для поиска.");
    return;
  }
  if (fingerprint) {
    if (acoustidKey.isEmpty() ||
        (fpcalcPath.isEmpty() &&
         QStandardPaths::findExecutable("fpcalc").isEmpty())) {
      QMessageBox::information(
          this, "AcoustID / Chromaprint",
          "Для распознавания нужен собственный application/client key AcoustID "
          "и локальный fpcalc. Настройки откроются сейчас.");
      searchSettings();
      if (acoustidKey.isEmpty() ||
          (fpcalcPath.isEmpty() &&
           QStandardPaths::findExecutable("fpcalc").isEmpty()))
        return;
    }
    if (QMessageBox::question(
            this, "Распознавание выбранных файлов",
            QString("Файлов: %1. Chromaprint локально вычислит отпечаток "
                    "первых 120 секунд каждого файла. В AcoustID уйдут "
                    "отпечаток и длительность; сам аудиофайл, путь и теги не "
                    "отправляются. Все результаты потребуют ручного выбора и "
                    "подтверждения. Продолжить?")
                .arg(searchQueue.size())) != QMessageBox::Yes)
      return;
    acoustid->configure(acoustidKey, fpcalcPath);
  }
  activeSource = fingerprint ? static_cast<MetadataSource *>(acoustid) : source;
  QSettings settings;
  QString contact =
      settings
          .value("musicbrainz/contact", "https://github.com/SweeetDozer/nmm")
          .toString();
  message(fingerprint ? "AcoustID: передаются только отпечаток и длительность; "
                        "аудио и текстовые теги не отправляются."
                      : "MusicBrainz: передаются поисковые название и "
                        "исполнитель из тегов / имён.");
  source->setContact(contact);
  searchChanges.clear();
  searchDone = 0;
  searchTotal = searchQueue.size();
  cancelled = false;
  setBusy(true);
  progress->setRange(0, searchTotal);
  nextSearch();
}
void Window::nextSearch() {
  if (cancelled || searchQueue.isEmpty()) {
    searchTotal = 0;
    setBusy(false);
    preview(searchChanges);
    return;
  }
  auto t = searchQueue.takeFirst();
  trackJournal.clear();
  status->setText(
      QString((activeSource == acoustid ? "AcoustID" : "MusicBrainz") +
              QString(" %1 / %2 · %3"))
          .arg(searchDone + 1)
          .arg(searchTotal)
          .arg(QFileInfo(t.path).fileName()));
  activeSource->lookup(t);
}
void Window::chooseCandidates(const Track &t,
                              const QList<Candidate> &candidates,
                              const QString &error) {
  if (cancelled)
    return;
  ++searchDone;
  progress->setValue(searchDone);
  if (!error.isEmpty())
    message("Ошибка источника: " + error + " · " + t.path);
  auto current = readTrack(t.path);
  if (!current.error.isEmpty()) {
    message(current.error);
    QTimer::singleShot(0, this, &Window::nextSearch);
    return;
  }
  RecordingDialog dialog(
      current, candidates, *online, this,
      trackJournal.join("\n") +
          (error.isEmpty() ? QString() : "\nОШИБКА: " + error));
  if (dialog.exec() == QDialog::Accepted)
    searchChanges << dialog.proposal();
  if (dialog.stopRequested())
    cancelled = true;
  QTimer::singleShot(0, this, &Window::nextSearch);
}
void Window::searchDiagnostic(const QString &text) {
  QString safe = text;
  if (!acoustidKey.isEmpty())
    safe.replace(acoustidKey, "[ключ скрыт]");
  if (!discogsToken.isEmpty())
    safe.replace(discogsToken, "[токен скрыт]");
  trackJournal << safe;
  searchJournal << QTime::currentTime().toString("HH:mm:ss") + "  " + safe;
  while (searchJournal.size() > 10000)
    searchJournal.removeFirst();
  message(safe);
}
void Window::showSearchJournal() {
  QDialog dialog(this);
  dialog.setWindowTitle("Журнал поиска · последние 10000 сообщений");
  dialog.resize(1000, 700);
  auto layout = new QVBoxLayout(&dialog);
  auto text = new QPlainTextEdit;
  text->setReadOnly(true);
  text->setPlainText(searchJournal.join("\n"));
  layout->addWidget(text);
  QTimer refresh;
  connect(&refresh, &QTimer::timeout, &dialog, [&, this] {
    if (text->toPlainText() != searchJournal.join("\n"))
      text->setPlainText(searchJournal.join("\n"));
  });
  refresh.start(500);
  auto buttons = new QDialogButtonBox(QDialogButtonBox::Close);
  connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
  layout->addWidget(buttons);
  dialog.exec();
}
void Window::searchSettings() {
  QDialog dialog(this);
  dialog.setWindowTitle("Настройки источников поиска");
  dialog.resize(760, 430);
  auto layout = new QVBoxLayout(&dialog);
  auto form = new QFormLayout;
  auto key = new QLineEdit(acoustidKey);
  key->setEchoMode(QLineEdit::Password);
  auto token = new QLineEdit(discogsToken);
  token->setEchoMode(QLineEdit::Password);
  auto executable = new QLineEdit(fpcalcPath);
  executable->setPlaceholderText(
      "fpcalc из PATH или полный путь к исполняемому файлу");
  form->addRow("AcoustID application/client key", key);
  form->addRow("Discogs personal access token", token);
  form->addRow("Chromaprint fpcalc", executable);
  layout->addLayout(form);
  auto browse = new QPushButton("Выбрать fpcalc…");
  layout->addWidget(browse);
  connect(browse, &QPushButton::clicked, &dialog, [&] {
    auto path =
        QFileDialog::getOpenFileName(&dialog, "Исполняемый файл fpcalc");
    if (!path.isEmpty())
      executable->setText(path);
  });
  auto info = new QLabel(
      "AcoustID: нужен ключ зарегистрированного приложения (client), не "
      "пользовательский ключ отправки отпечатков. Бесплатный сервис "
      "предназначен для некоммерческого использования. Fedora: sudo dnf "
      "install chromaprint-tools.\nDiscogs: токен нужен для API-поиска; без "
      "него доступна загрузка издания по ID и поиск в браузере.\nКлючи не "
      "попадают в журнал. По умолчанию действуют только в этой сессии.");
  info->setWordWrap(true);
  layout->addWidget(info);
  auto links = new QLabel(
      "<a href=\"https://acoustid.org/new-application\">Зарегистрировать "
      "приложение AcoustID</a> · <a "
      "href=\"https://www.discogs.com/settings/developers\">Токен Discogs</a>");
  links->setOpenExternalLinks(true);
  layout->addWidget(links);
  auto persist = new QCheckBox(
      "Сохранить ключи на этом компьютере в QSettings (без шифрования)");
  QSettings settings;
  persist->setChecked(settings.contains("acoustid/clientKey") ||
                      settings.contains("discogs/token"));
  layout->addWidget(persist);
  auto buttons =
      new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
  layout->addWidget(buttons);
  connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
  if (dialog.exec() != QDialog::Accepted)
    return;
  acoustidKey = key->text().trimmed();
  discogsToken = token->text().trimmed();
  fpcalcPath = executable->text().trimmed();
  if (persist->isChecked()) {
    settings.setValue("acoustid/clientKey", acoustidKey);
    settings.setValue("discogs/token", discogsToken);
  } else {
    settings.remove("acoustid/clientKey");
    settings.remove("discogs/token");
  }
  settings.setValue("chromaprint/executable", fpcalcPath);
  settings.sync();
#ifndef Q_OS_WIN
  QFile::setPermissions(settings.fileName(),
                        QFileDevice::ReadOwner | QFileDevice::WriteOwner);
#endif
  acoustid->configure(acoustidKey, fpcalcPath);
}
void Window::findDiscogs() {
  auto chosen = selected();
  if (chosen.size() != 1) {
    message("Для Discogs выберите один трек: издание и позицию в треклисте "
            "нужно проверить вручную.");
    return;
  }
  auto track = readTrack(chosen.first().path);
  if (!track.error.isEmpty()) {
    message(track.error);
    return;
  }
  DiscogsDialog dialog(track, *discogs, discogsToken, this);
  const auto result = dialog.exec();
  for (auto &line : dialog.journal())
    searchDiagnostic(line);
  if (result == QDialog::Accepted) {
    auto change = dialog.proposal();
    if (change)
      preview({*change});
  }
}
void Window::findLyrics() {
  const auto tracks = selected();
  if (tracks.size() != 1)
    return;
  const auto track = readTrack(tracks.first().path);
  if (!track.error.isEmpty()) {
    message(track.error);
    return;
  }
  LyricsDialog dialog(track, *online, this);
  if (dialog.exec() == QDialog::Accepted) {
    auto change = dialog.proposal();
    if (change)
      preview({*change});
  }
}
void Window::duplicates() {
  if (model->tracks.isEmpty())
    return;
  setBusy(true);
  cancelled = false;
  progress->setRange(0, model->tracks.size());
  progress->setValue(0);
  status->setText("Проверка повторов: SHA-256 и признаки записи…");
  auto tracks = model->tracks;
  job = QtConcurrent::run([this, tracks] {
    QMap<qint64, QList<Track>> bySize;
    for (auto &t : tracks)
      if (t.error.isEmpty())
        bySize[t.size] << t;
    QMap<QString, QList<Track>> hashes;
    QList<QString> errors;
    int count = 0;
    for (auto it = bySize.begin(); it != bySize.end(); ++it) {
      for (auto &t : it.value()) {
        if (cancelled)
          break;
        if (it.value().size() > 1) {
          auto hash = fileHash(t.path, &cancelled);
          if (!hash.isEmpty())
            hashes[QString::fromLatin1(hash.toHex())] << t;
          else if (!cancelled)
            errors << "Ошибка чтения для SHA-256: " + t.path;
        }
        ++count;
        if (count % 25 == 0)
          QMetaObject::invokeMethod(
              this, [this, count] { progress->setValue(count); });
      }
      if (cancelled)
        break;
    }
    QList<QPair<QString, QList<Track>>> groups;
    QHash<QString, QString> exactHashes;
    for (auto it = hashes.begin(); it != hashes.end(); ++it)
      if (it.value().size() > 1) {
        groups.append(
            {"Точные копии · SHA-256 " + it.key().left(12), it.value()});
        for (auto &t : it.value())
          exactHashes[t.path] = it.key();
      }
    QMap<QString, QList<Track>> candidates;
    for (auto &t : tracks)
      if (t.error.isEmpty() && t.duration > 0) {
        QString title = normalized(t.value("TITLE"));
        QString name = normalized(cleanTitle(QFileInfo(t.path).fileName()));
        if (!title.isEmpty())
          candidates["tag:" + normalized(t.value("ARTIST")) + "|" + title] << t;
        if (!name.isEmpty())
          candidates["file:" + name] << t;
      }
    QSet<QString> seen;
    for (auto list : candidates) {
      if (cancelled)
        break;
      std::sort(list.begin(), list.end(), [](const Track &a, const Track &b) {
        return a.duration < b.duration;
      });
      for (int i = 0; i < list.size();) {
        QList<Track> group;
        int j = i;
        while (j < list.size() && list[j].duration - list[i].duration <= 2000)
          group << list[j++];
        i = j;
        if (group.size() < 2)
          continue;
        bool allExact = true;
        for (auto &t : group)
          if (!exactHashes.contains(t.path) ||
              exactHashes.value(group.first().path) !=
                  exactHashes.value(t.path))
            allExact = false;
        if (allExact)
          continue;
        QStringList paths;
        for (auto &t : group)
          paths << t.path;
        paths.sort();
        QString key = paths.join("\n");
        if (seen.contains(key))
          continue;
        seen.insert(key);
        groups.append({"Возможные повторы · похожие теги / имена, длительность "
                       "±2 с · требуют прослушивания",
                       group});
      }
    }
    QMetaObject::invokeMethod(this, [this, groups, errors] {
      setBusy(false);
      for (auto &e : errors)
        message(e);
      if (cancelled) {
        message("Поиск повторов отменён.");
        return;
      }
      status->setText(QString("Групп повторов: %1").arg(groups.size()));
      if (groups.isEmpty()) {
        message("Повторы по выбранным признакам не найдены.");
        return;
      }
      QDialog d(this);
      d.setWindowTitle("Разбор повторов — отметьте лишние файлы");
      d.resize(1150, 650);
      auto v = new QVBoxLayout(&d);
      auto help =
          new QLabel("Похожие записи не являются доказанными дублями. Отметьте "
                     "только лишние файлы; в каждой группе должен остаться "
                     "хотя бы один. Двойной щелчок — слушать.");
      help->setWordWrap(true);
      v->addWidget(help);
      auto tree = new QTreeWidget;
      tree->setHeaderLabels(
          {"Файл / группа", "Время", "Формат", "кбит/с", "Гц", "Каналы"});
      v->addWidget(tree, 1);
      QHash<QString, Track> indexed;
      for (auto &g : groups) {
        auto parent = new QTreeWidgetItem(tree, {g.first});
        parent->setFirstColumnSpanned(true);
        for (auto &t : g.second) {
          auto row =
              new QTreeWidgetItem(parent, {t.path, duration(t.duration),
                                           t.format, QString::number(t.bitrate),
                                           QString::number(t.sampleRate),
                                           QString::number(t.channels)});
          row->setCheckState(0, Qt::Unchecked);
          row->setData(0, Qt::UserRole, t.path);
          indexed[t.path] = t;
        }
        parent->setExpanded(true);
      }
      tree->setColumnWidth(0, 700);
      connect(tree, &QTreeWidget::itemDoubleClicked, &d,
              [this](QTreeWidgetItem *i, int) {
                auto p = i->data(0, Qt::UserRole).toString();
                if (!p.isEmpty())
                  play(p);
              });
      auto b =
          new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
      b->button(QDialogButtonBox::Ok)
          ->setText("Перенести выбранные в папку восстановления");
      v->addWidget(b);
      connect(b, &QDialogButtonBox::rejected, &d, &QDialog::reject);
      QList<Track> chosen;
      connect(b, &QDialogButtonBox::accepted, &d, [&] {
        QSet<QString> paths;
        for (int i = 0; i < tree->topLevelItemCount(); ++i) {
          auto g = tree->topLevelItem(i);
          for (int j = 0; j < g->childCount(); ++j)
            if (g->child(j)->checkState(0) == Qt::Checked)
              paths.insert(g->child(j)->data(0, Qt::UserRole).toString());
        }
        if (paths.isEmpty())
          return;
        for (int i = 0; i < tree->topLevelItemCount(); ++i) {
          auto g = tree->topLevelItem(i);
          bool all = true;
          for (int j = 0; j < g->childCount(); ++j)
            if (!paths.contains(g->child(j)->data(0, Qt::UserRole).toString()))
              all = false;
          if (all) {
            QMessageBox::warning(&d, "Оставьте один файл",
                                 "Выбраны все файлы одной из групп, в том "
                                 "числе через пересекающиеся группы.");
            return;
          }
        }
        QMessageBox confirm(
            QMessageBox::Question, "Подтвердите перенос",
            QString("Файлов: %1\nПапка восстановления: %2/quarantine\nКопии "
                    "будут проверены перед удалением исходников. Полный список "
                    "— в подробностях.")
                .arg(paths.size())
                .arg(storage),
            QMessageBox::Yes | QMessageBox::No, &d);
        confirm.setDetailedText(QStringList(paths.values()).join("\n"));
        if (confirm.exec() != QMessageBox::Yes)
          return;
        for (auto &p : paths)
          chosen << indexed[p];
        d.accept();
      });
      if (d.exec() != QDialog::Accepted)
        return;
      player->stop();
      setBusy(true);
      cancelled = false;
      progress->setRange(0, chosen.size());
      job = QtConcurrent::run([this, chosen] {
        int n = 0;
        QStringList moved;
        for (auto &t : chosen) {
          if (cancelled)
            break;
          QString error;
          auto dest = quarantineFile(t, storage + "/quarantine", error);
          if (error.isEmpty())
            moved << t.path;
          ++n;
          QMetaObject::invokeMethod(this, [this, t, dest, error, n] {
            message(t.path + " → " + (error.isEmpty() ? dest : error));
            progress->setValue(n);
          });
        }
        QMetaObject::invokeMethod(this, [this, moved] {
          QList<Track> remaining;
          for (auto &t : model->tracks)
            if (!moved.contains(t.path))
              remaining << t;
          model->clear();
          model->append(remaining);
          setBusy(false);
          status->setText(QString("Перенесено в папку восстановления: %1")
                              .arg(moved.size()));
        });
      });
    });
  });
}
