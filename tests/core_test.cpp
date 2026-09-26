#include "core.h"
#include <QGuiApplication>
#include <QImage>
#include <iostream>
#include <memory>
#include <taglib/fileref.h>
#include <taglib/id3v2tag.h>
#include <taglib/mpegfile.h>
#include <taglib/privateframe.h>
#include <taglib/tpropertymap.h>

std::unique_ptr<TagLib::MPEG::File> mp3(const QString &path) {
#ifdef Q_OS_WIN
  return std::make_unique<TagLib::MPEG::File>(
      reinterpret_cast<const wchar_t *>(path.utf16()));
#else
  auto bytes = QFile::encodeName(path);
  return std::make_unique<TagLib::MPEG::File>(bytes.constData());
#endif
}

int main(int argc, char **argv) {
  QGuiApplication app(argc, argv);
  if (argc < 2)
    return 2;
  QString root = app.arguments().at(1);
  int failures = 0;
  auto check = [&](bool ok, QString msg) {
    if (!ok) {
      std::cerr << msg.toStdString() << '\n';
      ++failures;
    }
  };
  QImage image(8, 8, QImage::Format_RGB32);
  image.fill(Qt::red);
  QByteArray art;
  QBuffer buffer(&art);
  buffer.open(QIODevice::WriteOnly);
  image.save(&buffer, "PNG");
  for (QString ext : {"mp3", "flac", "ogg"}) {
    QString path = root + "/Исполнитель — тест." + ext;
    if (ext == "mp3") {
      auto file = mp3(path);
      auto frame = new TagLib::ID3v2::PrivateFrame;
      frame->setOwner("musicorder-test");
      frame->setData(TagLib::ByteVector("opaque-data", 11));
      file->ID3v2Tag(true)->addFrame(frame);
      check(file->save(), "Seed opaque ID3 frame");
    }
    auto before = readTrack(path);
    check(before.error.isEmpty(), "Read " + ext + ": " + before.error);
    if (!before.error.isEmpty())
      continue;
    auto original = fileHash(path);
    Change c{before,
             {{"TITLE", "Песня ё №1"},
              {"ARTIST", "Редкий автор"},
              {"ALBUM", "Тестовый альбом"},
              {"ALBUMARTIST", "Автор альбома"},
              {"TRACKNUMBER", "3"},
              {"DATE", "2024"},
              {"GENRE", "Electronic"},
              {"COMMENT", "Комментарий"},
              {"LYRICS", "Первая строка\nВторая строка"}},
             art};
    QFile obstruction(root + "/not-a-directory");
    if (!obstruction.exists()) {
      check(obstruction.open(QIODevice::WriteOnly), "Create obstruction");
      obstruction.write("x");
      obstruction.close();
    }
    auto blocked = writeTrack(c, root + "/not-a-directory/backups");
    check(!blocked.error.isEmpty() && fileHash(path) == original,
          "Backup failure leaves original intact " + ext);
    auto r = writeTrack(c, root + "/backups");
    check(r.error.isEmpty(), "Write " + ext + ": " + r.error);
    check(fileHash(r.backup) == original, "Backup exact " + ext);
    check(r.after.value("TITLE") == "Песня ё №1", "Unicode " + ext);
    check(r.after.cover == art, "Cover " + ext);
    check(r.after.value("COMPOSER") == before.value("COMPOSER"),
          "Preserve composer " + ext);
    check(r.after.duration == before.duration, "Duration unchanged " + ext);
    auto stale = writeTrack(c, root + "/backups");
    check(!stale.error.isEmpty(), "Reject stale preview " + ext);
    auto second =
        writeTrack(Change{readTrack(path),
                          {{"TITLE", "Второе название"},
                           {"ALBUM", "Выбранное издание"},
                           {"ALBUMARTIST", "Другой альбомный исполнитель"},
                           {"DATE", "2024-03-02"},
                           {"TRACKNUMBER", "7"},
                           {"DISCNUMBER", "2"}},
                          {}},
                   root + "/backups");
    check(second.error.isEmpty(), "Second write " + ext + second.error);
    check(second.after.cover == art, "Preserve cover " + ext);
    check(second.after.value("LYRICS") == c.fields["LYRICS"],
          "Preserve lyrics " + ext);
    check(second.after.pictureCount == 1, "Preserve artwork count " + ext);
    QImage otherImage(10, 10, QImage::Format_RGB32);
    otherImage.fill(Qt::blue);
    QByteArray otherArt;
    QBuffer otherBuffer(&otherArt);
    otherBuffer.open(QIODevice::WriteOnly);
    otherImage.save(&otherBuffer, "PNG");
    auto artworkOnly =
        writeTrack(Change{readTrack(path), {}, otherArt}, root + "/backups");
    check(artworkOnly.error.isEmpty(),
          "Artwork-only write " + ext + artworkOnly.error);
    check(artworkOnly.after.tags == second.after.tags,
          "Artwork-only preserves all tags " + ext);
    check(artworkOnly.after.cover == otherArt, "Artwork-only verified " + ext);
    auto lyricOnly =
        writeTrack(Change{readTrack(path),
                          {{"LYRICS", "Новый обычный текст для проверки"}},
                          {}},
                   root + "/backups");
    check(lyricOnly.error.isEmpty(),
          "Lyrics-only write " + ext + lyricOnly.error);
    check(lyricOnly.after.cover == otherArt, "Lyrics preserve artwork " + ext);
    check(lyricOnly.after.value("ALBUM") == "Выбранное издание",
          "Lyrics preserve selected release " + ext);
    auto removed =
        writeTrack(Change{readTrack(path), {{"COMMENT", ""}}, QByteArray()},
                   root + "/backups");
    check(removed.error.isEmpty(), "Delete fields " + ext + removed.error);
    check(removed.after.cover.isEmpty(), "Delete cover " + ext);
    if (ext == "mp3") {
      auto file = mp3(path);
      const auto frames = file->ID3v2Tag()->frameList("PRIV");
      check(frames.size() == 1, "Preserve opaque ID3 frame");
      if (!frames.isEmpty()) {
        auto frame =
            dynamic_cast<TagLib::ID3v2::PrivateFrame *>(frames.front());
        check(frame && frame->data() == TagLib::ByteVector("opaque-data", 11),
              "Preserve opaque payload");
      }
    }
    QString error;
    auto hash = fileHash(path);
    auto moved = quarantineFile(readTrack(path), root + "/quarantine", error);
    check(error.isEmpty() && !QFile::exists(path) && fileHash(moved) == hash,
          "Verified quarantine " + ext + error);
    check(QFile::copy(moved, path), "Restore quarantine " + ext);
  }
  check(!readTrack(root + "/broken.mp3").error.isEmpty(),
        "Malformed file rejected");
  return failures ? 1 : 0;
}
