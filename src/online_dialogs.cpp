#include "online_dialogs.h"
namespace {
QLabel *plainLabel(const QString &text = {}) {
  auto label = new QLabel(text);
  label->setTextFormat(Qt::PlainText);
  label->setWordWrap(true);
  label->setTextInteractionFlags(Qt::TextSelectableByMouse);
  return label;
}
QTableWidgetItem *readOnly(const QString &text) {
  auto i = new QTableWidgetItem(text);
  i->setFlags(i->flags() & ~Qt::ItemIsEditable);
  return i;
}
QString available(const QString &s) { return s.isEmpty() ? QString("—") : s; }
} // namespace
void showFullImage(QWidget *parent, const QByteArray &data,
                   const QString &source) {
  QPixmap pix;
  if (!pix.loadFromData(data))
    return;
  QDialog dialog(parent);
  dialog.setWindowTitle("Обложка · оригинальный размер");
  dialog.resize(900, 750);
  auto v = new QVBoxLayout(&dialog);
  v->addWidget(plainLabel(source + QString("\n%1 × %2 · %3 байт · масштаб 100%")
                                       .arg(pix.width())
                                       .arg(pix.height())
                                       .arg(data.size())));
  auto scroll = new QScrollArea;
  auto label = new QLabel;
  label->setPixmap(pix);
  label->resize(pix.size());
  scroll->setWidget(label);
  v->addWidget(scroll, 1);
  auto close = new QDialogButtonBox(QDialogButtonBox::Close);
  QObject::connect(close, &QDialogButtonBox::rejected, &dialog,
                   &QDialog::reject);
  v->addWidget(close);
  dialog.exec();
}
RecordingDialog::RecordingDialog(const Track &t, const QList<Candidate> &items,
                                 OnlineServices &s, QWidget *parent,
                                 QString diagnostics)
    : QDialog(parent), track(t), candidates(items), services(s) {
  setObjectName("recordingDialog");
  setWindowTitle("Запись → конкретное издание → предпросмотр");
  resize(1150, 850);
  auto v = new QVBoxLayout(this);
  v->addWidget(plainLabel(t.path +
                          "\nВыберите запись. Альбом необязателен: ни одно "
                          "издание не выбирается автоматически."));
  recordings = new QListWidget;
  recordings->setObjectName("recordingCandidates");
  recordings->setWordWrap(true);
  recordings->setMaximumHeight(195);
  for (auto &c : candidates)
    recordings->addItem(
        c.fields.value("ARTIST") + " — " + c.fields.value("TITLE") + "\n" +
        c.source +
        (c.reliable ? " · сильное совпадение" : " · требуется проверка") +
        "\n" + c.reason);
  v->addWidget(recordings);
  if (!diagnostics.isEmpty()) {
    auto journal = new QPlainTextEdit;
    journal->setObjectName("candidateSearchLog");
    journal->setReadOnly(true);
    journal->setPlainText(diagnostics);
    journal->setMaximumHeight(150);
    v->addWidget(journal);
  }
  if (candidates.isEmpty())
    v->addWidget(
        plainLabel("Предложений нет. Причина показана в журнале: пустой ответ, "
                   "ошибка или недостаточно данных — это разные ситуации."));
  state = plainLabel(
      "MusicBrainz получает идентификатор записи; Cover Art Archive — "
      "идентификатор выбранного издания. Аудиофайл не отправляется.");
  state->setObjectName("releaseStatus");
  v->addWidget(state);
  editions = new QTableWidget(0, 9);
  editions->setObjectName("releaseCandidates");
  editions->setHorizontalHeaderLabels(
      {"Альбом", "Исполнитель альбома", "Дата", "Тип / статус", "Страна",
       "Носитель / диск", "№ трека", "Уточнение", "MBID"});
  editions->setSelectionBehavior(QAbstractItemView::SelectRows);
  editions->setSelectionMode(QAbstractItemView::SingleSelection);
  editions->horizontalHeader()->setSectionResizeMode(
      QHeaderView::ResizeToContents);
  v->addWidget(editions, 1);
  auto controls = new QHBoxLayout;
  auto none = new QPushButton("Без альбома и обложки");
  none->setObjectName("noRelease");
  more = new QPushButton("Ещё издания");
  more->setEnabled(false);
  controls->addWidget(none);
  controls->addWidget(more);
  v->addLayout(controls);
  auto artwork = new QHBoxLayout;
  art = new QLabel("Обложка ещё не предложена");
  art->setObjectName("coverThumbnail");
  art->setAlignment(Qt::AlignCenter);
  art->setFixedSize(175, 175);
  art->setWordWrap(true);
  artwork->addWidget(art);
  auto detail = new QVBoxLayout;
  artSource =
      plainLabel("При выборе издания будет запрошена его лицевая обложка. Если "
                 "её нет, текущая останется без изменений.");
  artSource->setObjectName("coverStatus");
  detail->addWidget(artSource);
  full = new QPushButton("Открыть полный размер");
  full->setEnabled(false);
  detail->addWidget(full);
  detail->addWidget(
      plainLabel("Альбомные поля и обложка выбираются независимо на следующем "
                 "экране. Замена относится ко всем обложкам файла."));
  artwork->addLayout(detail, 1);
  v->addLayout(artwork);
  auto buttons = new QDialogButtonBox;
  apply = buttons->addButton("В предпросмотр", QDialogButtonBox::AcceptRole);
  apply->setObjectName("reviewRecording");
  apply->setEnabled(false);
  auto skip = buttons->addButton("Пропустить", QDialogButtonBox::RejectRole);
  auto finish =
      buttons->addButton("Завершить поиск", QDialogButtonBox::DestructiveRole);
  v->addWidget(buttons);
  connect(recordings, &QListWidget::currentRowChanged, this,
          &RecordingDialog::recordingChanged);
  connect(editions, &QTableWidget::itemSelectionChanged, this,
          &RecordingDialog::releaseChanged);
  connect(none, &QPushButton::clicked, this, [this] {
    editions->clearSelection();
    releaseChanged();
  });
  connect(more, &QPushButton::clicked, this, &RecordingDialog::loadPage);
  connect(full, &QPushButton::clicked, this, [this] {
    if (selectedCover)
      showFullImage(this, selectedCover->data, selectedCover->source);
  });
  connect(apply, &QPushButton::clicked, this, &QDialog::accept);
  connect(skip, &QPushButton::clicked, this, &QDialog::reject);
  connect(finish, &QPushButton::clicked, this, [this] {
    stop = true;
    reject();
  });
}
RecordingDialog::~RecordingDialog() {
  clearContext(releaseContext);
  clearContext(coverContext);
}
void RecordingDialog::clearContext(QObject *&context) {
  if (context) {
    services.cancel(context);
    delete context;
    context = nullptr;
  }
}
void RecordingDialog::recordingChanged(int row) {
  clearContext(releaseContext);
  clearContext(coverContext);
  selectedRelease.reset();
  selectedCover.reset();
  options.clear();
  offset = 0;
  {
    QSignalBlocker blocker(editions);
    editions->setRowCount(0);
  }
  art->setPixmap({});
  art->setText("Обложка не выбрана");
  full->setEnabled(false);
  more->setEnabled(false);
  artSource->setText("Без выбора издания текущая обложка не меняется.");
  apply->setEnabled(row >= 0);
  if (row < 0)
    return;
  if (candidates[row].recordingId.isEmpty()) {
    state->setText("Локальная подсказка: связанные издания неизвестны.");
    return;
  }
  releaseContext = new QObject(this);
  loadPage();
}
void RecordingDialog::loadPage() {
  int row = recordings->currentRow();
  if (row < 0 || !releaseContext)
    return;
  more->setEnabled(false);
  state->setText(
      "Загрузка изданий… Можно продолжить только с названием и исполнителем.");
  services.releases(
      candidates[row].recordingId, offset, releaseContext,
      [this](ReleasePage page) {
        if (!page.error.isEmpty()) {
          state->setText("Издания: " + page.error);
          more->setEnabled(true);
          return;
        }
        offset = page.nextOffset;
        QSignalBlocker blocker(editions);
        for (auto &option : page.options) {
          options << option;
          int r = editions->rowCount();
          editions->insertRow(r);
          QStringList columns = {
              option.album,       option.albumArtist,
              option.date,        option.type + " / " + option.status,
              option.country,     option.format + " / " + option.discNumber,
              option.trackNumber, option.disambiguation,
              option.id};
          for (int c = 0; c < columns.size(); ++c)
            editions->setItem(r, c, readOnly(available(columns[c])));
        }
        state->setText(
            QString(
                "Просмотрено изданий: %1 / %2. Выберите строку или продолжите "
                "без альбома. Вхождения на разных дисках показаны отдельно.")
                .arg(offset)
                .arg(page.total));
        more->setEnabled(offset < page.total);
      });
}
void RecordingDialog::releaseChanged() {
  clearContext(coverContext);
  selectedCover.reset();
  selectedRelease.reset();
  full->setEnabled(false);
  art->setPixmap({});
  const auto rows = editions->selectionModel()->selectedRows();
  if (rows.isEmpty()) {
    art->setText("Обложка не выбрана");
    artSource->setText("Текущая обложка останется без изменений.");
    return;
  }
  auto option = options.at(rows.first().row());
  selectedRelease = option;
  art->setText("Загрузка…");
  artSource->setText(
      "Cover Art Archive · выбранное издание: " + option.id +
      "\nЗагрузка ограничена 20 МБ. Можно продолжить без обложки.");
  coverContext = new QObject(this);
  services.cover(option.id, coverContext, [this](CoverOffer offer) {
    if (!selectedRelease || selectedRelease->id != offer.releaseId)
      return;
    selectedCover = offer;
    if (offer.absent || !offer.error.isEmpty()) {
      art->setText(offer.absent ? "Нет лицевой обложки" : "Ошибка обложки");
      artSource->setText(
          offer.source + "\n" +
          (offer.absent ? "У выбранного издания лицевая обложка не найдена."
                        : offer.error) +
          "\nСуществующая обложка не изменится.");
      return;
    }
    QPixmap pix;
    pix.loadFromData(offer.data);
    art->setPixmap(
        pix.scaled(175, 175, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    artSource->setText(
        offer.source +
        QString(
            "\n%1 × %2 · %3 байт. Это предложение; запись ещё не выполняется.")
            .arg(pix.width())
            .arg(pix.height())
            .arg(offer.data.size()));
    full->setEnabled(true);
  });
}
Change RecordingDialog::proposal() const {
  int row = recordings->currentRow();
  if (row < 0)
    return {track, {}, {}, {}};
  return selectedProposal(track, candidates[row], selectedRelease,
                          selectedCover);
}

LyricsDialog::LyricsDialog(const Track &t, OnlineServices &s, QWidget *parent)
    : QDialog(parent), track(t), services(s) {
  setObjectName("lyricsDialog");
  setWindowTitle("Необязательный поиск текста · один трек");
  resize(1000, 800);
  auto v = new QVBoxLayout(this);
  v->addWidget(plainLabel(
      t.path + "\nПо кнопке «Найти» LRCLIB получит название, исполнителя и "
               "непустой альбом. Длительность сравнивается локально. "
               "Аудиофайл, путь и существующий текст не отправляются."));
  auto form = new QFormLayout;
  title = new QLineEdit(t.value("TITLE"));
  artist = new QLineEdit(t.value("ARTIST"));
  album = new QLineEdit(t.value("ALBUM"));
  title->setObjectName("lyricTitle");
  artist->setObjectName("lyricArtist");
  album->setObjectName("lyricAlbum");
  form->addRow("Название для поиска", title);
  form->addRow("Исполнитель для поиска", artist);
  form->addRow("Альбом (необязательно)", album);
  v->addLayout(form);
  search = new QPushButton("Найти в LRCLIB");
  search->setObjectName("findLyrics");
  v->addWidget(search);
  state = plainLabel(QString("Длительность файла: %1 с. Допускается отклонение "
                             "до 2 с. Совпадения только названия недостаточно.")
                         .arg(t.duration / 1000.0, 0, 'f', 1));
  state->setObjectName("lyricsStatus");
  v->addWidget(state);
  list = new QListWidget;
  list->setObjectName("lyricCandidates");
  list->setWordWrap(true);
  list->setMaximumHeight(210);
  v->addWidget(list);
  text = new QPlainTextEdit;
  text->setObjectName("lyricPreview");
  text->setReadOnly(true);
  v->addWidget(text, 1);
  auto buttons = new QDialogButtonBox;
  apply =
      buttons->addButton("Текст в предпросмотр", QDialogButtonBox::AcceptRole);
  apply->setObjectName("reviewLyrics");
  apply->setEnabled(false);
  buttons->addButton(QDialogButtonBox::Cancel);
  v->addWidget(buttons);
  connect(search, &QPushButton::clicked, this, &LyricsDialog::startSearch);
  for (auto input : {title, artist, album})
    connect(input, &QLineEdit::textChanged, this, [this] { invalidate(); });
  connect(list, &QListWidget::currentRowChanged, this, [this](int row) {
    apply->setEnabled(row >= 0 && options.value(row).eligible);
    text->setPlainText(row < 0
                           ? QString()
                           : options[row].source + "\n" + options[row].reason +
                                 "\n\n" + options[row].text);
  });
  connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
}
LyricsDialog::~LyricsDialog() {
  if (context) {
    services.cancel(context);
    delete context;
  }
}
void LyricsDialog::invalidate() {
  if (context) {
    services.cancel(context);
    delete context;
    context = nullptr;
  }
  options.clear();
  list->clear();
  text->clear();
  apply->setEnabled(false);
  search->setEnabled(true);
}
void LyricsDialog::startSearch() {
  invalidate();
  if (title->text().trimmed().isEmpty() || artist->text().trimmed().isEmpty()) {
    state->setText("Для поиска нужны название и исполнитель.");
    return;
  }
  auto query = track;
  query.tags["TITLE"] = {title->text().trimmed()};
  query.tags["ARTIST"] = {artist->text().trimmed()};
  query.tags["ALBUM"] = {album->text().trimmed()};
  context = new QObject(this);
  search->setEnabled(false);
  state->setText("Поиск LRCLIB… Закрытие окна отменяет запрос.");
  services.lyrics(
      query, context, [this](QList<LyricOption> results, QString error) {
        search->setEnabled(true);
        options = results;
        if (!error.isEmpty()) {
          state->setText("LRCLIB: " + error +
                         ". Ничего не предложено к записи.");
          return;
        }
        state->setText(
            results.isEmpty()
                ? "Текст не найден. Существующее поле останется без изменений."
                : "Выберите результат и прочитайте текст. Неоднозначные версии "
                  "и синхронизированный LRC доступны только для просмотра.");
        for (auto &o : options)
          list->addItem(o.artist + " — " + o.title + " · " +
                        available(o.album) +
                        QString(" · %1 с").arg(o.duration, 0, 'f', 1) + "\n" +
                        (o.synced ? "Синхронизированный LRC (временные метки)"
                                  : "Обычный текст") +
                        (o.eligible ? " · можно добавить после проверки"
                                    : " · запись заблокирована") +
                        "\n" + o.source);
      });
}
std::optional<Change> LyricsDialog::proposal() const {
  int row = list->currentRow();
  if (row < 0 || !options[row].eligible || options[row].synced)
    return {};
  return Change{track,
                {{"LYRICS", options[row].text}},
                {},
                {{"LYRICS", options[row].source + " · обычный текст"}}};
}
