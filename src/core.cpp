#include "core.h"
#include <memory>
#include <taglib/audioproperties.h>
#include <taglib/fileref.h>
#include <taglib/flacfile.h>
#include <taglib/mpegfile.h>
#include <taglib/tpropertymap.h>
#include <taglib/tvariant.h>
#include <taglib/vorbisfile.h>
namespace {
TagLib::String ts(const QString &s) {
  return TagLib::String(s.toUtf8().constData(), TagLib::String::UTF8);
}
QString qs(const TagLib::String &s) {
  return QString::fromUtf8(s.toCString(true));
}
std::unique_ptr<TagLib::FileRef> openTag(const QString &path) {
#ifdef Q_OS_WIN
  return std::make_unique<TagLib::FileRef>(
      reinterpret_cast<const wchar_t *>(path.utf16()));
#else
  auto bytes = QFile::encodeName(path);
  return std::make_unique<TagLib::FileRef>(bytes.constData());
#endif
}
bool supported(TagLib::FileRef &f) {
  return dynamic_cast<TagLib::MPEG::File *>(f.file()) ||
         dynamic_cast<TagLib::FLAC::File *>(f.file()) ||
         dynamic_cast<TagLib::Ogg::Vorbis::File *>(f.file());
}
bool manifest(const QString &path, const QJsonObject &obj) {
  QSaveFile f(path);
  if (!f.open(QIODevice::WriteOnly))
    return false;
  return f.write(QJsonDocument(obj).toJson()) > 0 && f.commit();
}
} // namespace
QStringList editableFields() {
  return {"TITLE", "ARTIST", "ALBUM",   "ALBUMARTIST", "TRACKNUMBER",
          "DATE",  "GENRE",  "COMMENT", "LYRICS"};
}
Track readTrack(const QString &path, bool loadCover) {
  Track t;
  t.coverLoaded = loadCover;
  t.path = QFileInfo(path).absoluteFilePath();
  QFileInfo info(path);
  t.size = info.size();
  t.modified = info.lastModified().toMSecsSinceEpoch();
  t.format = info.suffix().toUpper();
  auto f = openTag(path);
  if (f->isNull() || !f->file()->isValid() || !supported(*f)) {
    t.error = "Не удалось прочитать MP3, FLAC или OGG Vorbis (Opus не "
              "поддерживается)";
    return t;
  }
  const auto props = f->properties();
  for (auto it = props.begin(); it != props.end(); ++it) {
    QStringList values;
    for (const auto &v : it->second)
      values << qs(v);
    t.tags[qs(it->first)] = values;
  }
  if (auto p = f->audioProperties()) {
    t.duration = p->lengthInMilliseconds();
    t.bitrate = p->bitrate();
    t.sampleRate = p->sampleRate();
    t.channels = p->channels();
  }
  if (t.sampleRate <= 0 || t.channels <= 0) {
    t.error = "Нет корректного аудиопотока или аудиопараметров";
    return t;
  }
  for (const auto &key : props.unsupportedData())
    t.unsupported << qs(key);
  for (const auto &key : f->complexPropertyKeys())
    t.complexKeys << qs(key);
  if (!loadCover)
    return t;
  auto pictures = f->complexProperties("PICTURE");
  t.pictureCount = pictures.size();
  if (!pictures.isEmpty()) {
    auto data = pictures.front()["data"].toByteVector();
    t.cover = QByteArray(data.data(), data.size());
  }
  return t;
}
QByteArray fileHash(const QString &path, std::atomic_bool *cancel) {
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly))
    return {};
  QCryptographicHash hash(QCryptographicHash::Sha256);
  while (!f.atEnd()) {
    if (cancel && *cancel)
      return {};
    auto bytes = f.read(1024 * 1024);
    if (bytes.isEmpty() && f.error() != QFile::NoError)
      return {};
    hash.addData(bytes);
  }
  return hash.result();
}
WriteResult writeTrack(const Change &c, const QString &root) {
  WriteResult r;
  r.path = c.before.path;
  auto current = readTrack(r.path);
  if (!current.error.isEmpty()) {
    r.error = current.error;
    return r;
  }
  if (current.size != c.before.size || current.modified != c.before.modified ||
      current.tags != c.before.tags ||
      (c.before.coverLoaded && current.cover != c.before.cover)) {
    r.error = "Файл изменился после предпросмотра. Обновите коллекцию.";
    return r;
  }
  QString dir =
      QDir(root).filePath(QUuid::createUuid().toString(QUuid::WithoutBraces));
  if (!QDir().mkpath(dir)) {
    r.error = "Не удалось создать папку резервной копии";
    return r;
  }
  r.backup = QDir(dir).filePath(QFileInfo(r.path).fileName());
  auto hash = fileHash(r.path);
  if (hash.isEmpty() || !QFile::copy(r.path, r.backup) ||
      fileHash(r.backup) != hash) {
    r.error = "Резервная копия не прошла проверку; запись отменена";
    return r;
  }
  if (!manifest(
          QDir(dir).filePath("restore.json"),
          {{"original", r.path},
           {"backup", r.backup},
           {"sha256", QString::fromLatin1(hash.toHex())},
           {"time", QDateTime::currentDateTimeUtc().toString(Qt::ISODate)}})) {
    r.error = "Не удалось записать журнал восстановления";
    return r;
  }
  {
    auto f = openTag(r.path);
    if (f->isNull()) {
      r.error = "Не удалось открыть файл для записи";
      return r;
    }
    auto props = f->properties();
    for (auto it = c.fields.begin(); it != c.fields.end(); ++it) {
      if (!editableFields().contains(it.key())) {
        r.error = "Поле не поддерживается редактором: " + it.key();
        return r;
      }
      if (it.value().isEmpty())
        props.erase(ts(it.key()));
      else
        props.replace(ts(it.key()), TagLib::StringList(ts(it.value())));
    }
    if (!f->setProperties(props).isEmpty()) {
      r.error = "TagLib отклонил часть полей; файл не сохранён";
      return r;
    }
    if (c.cover) {
      TagLib::List<TagLib::VariantMap> pictures;
      if (!c.cover->isEmpty()) {
        TagLib::VariantMap pic;
        pic["data"] = TagLib::ByteVector(c.cover->constData(), c.cover->size());
        pic["pictureType"] = TagLib::String("Front Cover");
        pic["description"] = TagLib::String("");
        pic["mimeType"] = TagLib::String(
            c.cover->startsWith("\x89PNG") ? "image/png" : "image/jpeg");
        pictures.append(pic);
      }
      if (!f->setComplexProperties("PICTURE", pictures)) {
        r.error = "Этот формат не поддерживает запись обложки";
        return r;
      }
    }
    if (!f->save())
      r.error = "Ошибка записи. Оригинал сохранён в резервной копии.";
  }
  r.after = readTrack(r.path);
  if (!r.after.error.isEmpty())
    r.error = r.after.error;
  for (auto it = c.fields.begin(); it != c.fields.end(); ++it)
    if (r.after.value(it.key()) != it.value())
      r.error += "\nПроверка не пройдена: " + it.key();
  if (c.cover && r.after.cover != *c.cover)
    r.error += "\nПроверка обложки не пройдена";
  for (auto it = current.tags.begin(); it != current.tags.end(); ++it)
    if (!c.fields.contains(it.key()) &&
        r.after.tags.value(it.key()) != it.value())
      r.error += "\nИзменилось невыбранное поле: " + it.key();
  return r;
}
QString quarantineFile(const Track &t, const QString &root, QString &error) {
  QFileInfo info(t.path);
  if (info.size() != t.size ||
      info.lastModified().toMSecsSinceEpoch() != t.modified) {
    error = "Файл изменился; повторите поиск";
    return {};
  }
  QString dir =
      QDir(root).filePath(QUuid::createUuid().toString(QUuid::WithoutBraces));
  if (!QDir().mkpath(dir)) {
    error = "Не удалось создать папку восстановления";
    return {};
  }
  QString dest = QDir(dir).filePath(info.fileName());
  auto hash = fileHash(t.path);
  if (hash.isEmpty() || !QFile::copy(t.path, dest) || fileHash(dest) != hash ||
      fileHash(t.path) != hash) {
    error = "Не удалось проверить копию; исходный файл оставлен";
    return {};
  }
  if (!manifest(QDir(dir).filePath("restore.json"),
                {{"original", t.path},
                 {"backup", dest},
                 {"sha256", QString::fromLatin1(hash.toHex())}})) {
    error = "Не удалось записать журнал; исходник оставлен";
    return {};
  }
  if (!QFile::remove(t.path)) {
    error = "Копия создана, но исходный файл не удалось удалить";
    return {};
  }
  return dest;
}
QString cleanTitle(QString s) {
  s = QFileInfo(s).completeBaseName();
  s.replace(QRegularExpression("\\[[A-Za-z0-9_-]{11}\\]$"), "");
  s.replace(QRegularExpression("\\b(official\\s+(music\\s+)?video|official "
                               "audio|lyrics video|1080p|720p)\\b",
                               QRegularExpression::CaseInsensitiveOption),
            "");
  return s.simplified();
}
QString normalized(QString s) {
  return s.normalized(QString::NormalizationForm_KC)
      .toCaseFolded()
      .simplified();
}
QMap<QString, QString> localSuggestion(const Track &t) {
  return {{"TITLE", cleanTitle(QFileInfo(t.path).fileName())},
          {"ARTIST", QFileInfo(t.path).dir().dirName()}};
}
