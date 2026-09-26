#include "window.h"
int main(int argc, char **argv) {
  QApplication app(argc, argv);
  QCoreApplication::setOrganizationName("MusicOrder");
  QCoreApplication::setApplicationName("MusicOrder");
  QCoreApplication::setApplicationVersion("0.1.0");
  Window window;
  window.show();
  if (app.arguments().contains("--smoke-test"))
    QTimer::singleShot(500, &app, &QApplication::quit);
  return app.exec();
}
