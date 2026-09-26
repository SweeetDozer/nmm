#include "window.h"
#include <QtTest>
class UiTest : public QObject {
  Q_OBJECT
private slots:
  void scanCollection() {
    QStandardPaths::setTestModeEnabled(true);
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    QString sample = temp.path() + "/sample.mp3";
    QProcess generator;
    generator.start("ffmpeg", {"-v", "error", "-f", "lavfi", "-i",
                               "sine=frequency=440:duration=0.05", "-c:a",
                               "libmp3lame", "-metadata", "artist=Исполнитель",
                               "-metadata", "title=Название", sample});
    QVERIFY(generator.waitForFinished());
    QCOMPARE(generator.exitCode(), 0);
    QString folder = temp.path() + "/Музыка";
    QVERIFY(QDir().mkpath(folder));
    for (int i = 0; i < 5000; ++i)
      QVERIFY(QFile::copy(sample, folder + QString("/Трек %1.MP3").arg(i)));
    QFile bad(folder + "/broken.ogg");
    QVERIFY(bad.open(QIODevice::WriteOnly));
    bad.write("broken");
    bad.close();
    Window window;
    window.show();
    QSignalSpy loaded(&window, &Window::collectionLoaded);
    int ticks = 0;
    QTimer heartbeat;
    connect(&heartbeat, &QTimer::timeout, [&] { ++ticks; });
    heartbeat.start(10);
    window.openCollection(folder);
    QVERIFY(loaded.wait(60000));
    QCOMPARE(loaded.first().first().toInt(), 5001);
    QVERIFY2(ticks > 2, "UI event loop should run during scan");
    auto table = window.findChild<QTableView *>();
    QVERIFY(table);
    QCOMPARE(table->model()->rowCount(), 5001);
    auto filter = window.findChild<QComboBox *>();
    QVERIFY(filter);
    filter->setCurrentIndex(2);
    QCOMPARE(table->model()->rowCount(), 1);
    filter->setCurrentIndex(0);
    auto search = window.findChild<QLineEdit *>();
    QVERIFY(search);
    search->setText("Трек 4999");
    QCOMPARE(table->model()->rowCount(), 1);
    search->clear();
    table->sortByColumn(3, Qt::DescendingOrder);
    QCOMPARE(table->model()->rowCount(), 5001);
    window.grab().save(QDir::tempPath() + "/musicorder-ui.png");
    // Starting another scan immediately after completion must also be safe.
    loaded.clear();
    window.openCollection(folder);
    auto cancel = window.findChildren<QPushButton *>();
    for (auto button : cancel)
      if (button->text() == "Отмена")
        button->click();
    QVERIFY(loaded.wait(60000));
  }
};
QTEST_MAIN(UiTest)
#include "ui_test.moc"
