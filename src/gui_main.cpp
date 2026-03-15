// ─────────────────────────────────────────────────────────────────────────────
// prad2hvmon – PRad-II HV Monitor (Qt GUI client)
//
// A thin Qt WebEngine wrapper that loads the dashboard HTML and connects
// to the prad2hvd daemon via WebSocket.  No hardware access — all data
// comes from the daemon.
//
// Usage:
//   prad2hvmon                          # default: localhost:8765
//   prad2hvmon -H clonpc19              # connect to daemon on clonpc19
//   prad2hvmon -H clonpc19 -p 9000     # custom host and port
//   prad2hvmon -r /path/to/resources   # custom resources directory
//
// Author: Chao Peng — Argonne National Laboratory
// ─────────────────────────────────────────────────────────────────────────────

#include <QApplication>
#include <QWebEngineView>
#include <QWebEnginePage>
#include <QUrl>
#include <QUrlQuery>
#include <QFile>
#include <QDir>
#include <QCoreApplication>
#include <QShortcut>
#include <QKeySequence>
#include <QPixmap>
#include <QDateTime>
#include <QCommandLineParser>
#include <QCommandLineOption>

#include <iostream>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    app.setApplicationName("PRad-II HV Monitor");

    // ── Command-line options ─────────────────────────────────────────
    QCommandLineParser parser;
    parser.setApplicationDescription("PRad-II HV Monitor — Qt GUI client for prad2hvd");
    parser.addHelpOption();

    QCommandLineOption hostOpt(QStringList() << "H" << "host",
        "Daemon hostname (default: localhost).", "host", "localhost");
    QCommandLineOption portOpt(QStringList() << "p" << "port",
        "Daemon WebSocket port (default: 8765).", "port", "8765");
    QCommandLineOption resourceOpt(QStringList() << "r" << "resources",
        "Path to resources directory containing monitor.html.", "dir");
    QCommandLineOption widthOpt("width",
        "Window width (default: 1400).", "px", "1400");
    QCommandLineOption heightOpt("height",
        "Window height (default: 900).", "px", "900");

    parser.addOption(hostOpt);
    parser.addOption(portOpt);
    parser.addOption(resourceOpt);
    parser.addOption(widthOpt);
    parser.addOption(heightOpt);
    parser.process(app);

    QString host   = parser.value(hostOpt);
    QString port   = parser.value(portOpt);
    int winW       = parser.value(widthOpt).toInt();
    int winH       = parser.value(heightOpt).toInt();

    // ── Locate resources directory ───────────────────────────────────
    QString resourceDir;
    if (parser.isSet(resourceOpt)) {
        resourceDir = parser.value(resourceOpt);
    } else {
        // Auto-discover relative to the binary
        QStringList candidates = {
            QCoreApplication::applicationDirPath() + "/../resources",
            QCoreApplication::applicationDirPath() + "/../../resources",
#ifdef RESOURCE_DIR
            QString::fromStdString(RESOURCE_DIR),
#endif
        };
        for (const auto &p : candidates) {
            if (QFile::exists(p + "/monitor.html")) {
                resourceDir = QDir(p).absolutePath();
                break;
            }
        }
    }

    if (resourceDir.isEmpty() && parser.isSet(resourceOpt)) {
        std::cerr << "ERROR: cannot find monitor.html in specified resources dir\n";
        return 1;
    }
    if (!resourceDir.isEmpty())
        std::cout << "Resources: " << resourceDir.toStdString() << "\n";
    std::cout << "Daemon: " << host.toStdString() << ":" << port.toStdString() << "\n";

    // ── Create web view ──────────────────────────────────────────────
    QWebEngineView view;
    view.setWindowTitle("PRad-II HV Monitor");
    view.resize(winW, winH);

    // Load dashboard from daemon's built-in HTTP server.
    // The daemon serves resources at http://host:port/monitor.html
    // and WebSocket on the same port — no separate file server needed.
    QUrl url;
    if (parser.isSet(resourceOpt)) {
        // Explicit local resources — use file:// with query params
        url = QUrl::fromLocalFile(resourceDir + "/monitor.html");
        QUrlQuery query;
        query.addQueryItem("host", host);
        query.addQueryItem("port", port);
        url.setQuery(query);
    } else {
        // Load from daemon's HTTP server (default)
        url = QUrl(QString("http://%1:%2/monitor.html").arg(host, port));
    }
    std::cout << "Loading: " << url.toString().toStdString() << "\n";
    view.setUrl(url);

    // ── Screenshot shortcut (Ctrl+S) ─────────────────────────────────
    auto *screenshotShortcut = new QShortcut(QKeySequence("Ctrl+S"), &view);
    QObject::connect(screenshotShortcut, &QShortcut::activated, [&view]() {
        // Save screenshots to DATABASE_DIR/screenshots/
        const QString dir = QString::fromStdString(DATABASE_DIR) + "/screenshots";
        QDir().mkpath(dir);  // ensure the directory exists
        const QString ts  = QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss");
        const QString path = dir + "/prad2hvmon_" + ts + ".png";
        const QPixmap px = view.grab();
        if (px.save(path))
            std::cout << "Screenshot saved: " << path.toStdString() << "\n";
        else
            std::cerr << "Screenshot failed: " << path.toStdString() << "\n";
    });

    view.show();
    return app.exec();
}
