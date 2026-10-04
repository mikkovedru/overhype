#include "apptheme.h"
#include "cli.h"
#include "deck.h"
#include "renderer.h"
#include <QGuiApplication>
#include <QCommandLineParser>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusVariant>
#include <QDir>
#include <QCryptographicHash>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLockFile>
#include <QStandardPaths>
#include <QFileInfo>
#include <memory>
#include <QFont>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QScopeGuard>
#include <QTimer>
#include <cstdio>
// The desktop's interface font, e.g. "Adwaita Sans 11", which the gtk3 platform
// theme used to supply. Without a settings portal Qt's default font stays.
static void adoptDesktopFont() {
    auto call = QDBusMessage::createMethodCall("org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
                                               "org.freedesktop.portal.Settings", "ReadOne");
    call.setArguments({"org.gnome.desktop.interface", "font-name"});
    const auto reply = QDBusConnection::sessionBus().call(call, QDBus::Block, 500);
    if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty()) return;
    const QString name = reply.arguments().first().value<QDBusVariant>().variant().toString();
    const int space = name.lastIndexOf(' ');
    const double size = name.mid(space + 1).toDouble();
    if (space <= 0 || size <= 0) return;
    QFont font(name.left(space));
    font.setPointSizeF(size);
    QGuiApplication::setFont(font);
}
int main(int argc, char **argv) {
    QStringList arguments;
    for (int i = 0; i < argc; ++i) arguments.append(QString::fromLocal8Bit(argv[i]));
    const bool command = isCliCommand(arguments.value(1));
    const bool explicitResume = arguments.value(1) == "open";
    if (explicitResume) arguments.removeAt(1);
    QCommandLineParser args;
    args.setApplicationDescription("Simple Markdown presentations with a visual slide editor.\n\n" + cliSummary());
    args.addHelpOption();
    args.addVersionOption();
    args.addPositionalArgument("presentation", "Markdown presentation");
    args.addOption({"home", "Start on Home"});
    args.addOption({"new", "Start a fresh untitled presentation"});
    args.addOption({"open-dialog", "Choose a presentation to open"});
    args.addOption({"pdf", "Export PDF and exit", "file"});
    args.addOption({"pptx", "Export rendered PowerPoint and exit", "file"});
    args.addOption({"render", "Render slide PNGs and manifest and exit", "directory"});
    args.addOption({"theme", "Apply installed theme", "name"});
    args.addOption({"save", "Save changes (for theme snapshots)"});
    args.addOption({"slide", "Select a slide (1-based)", "number"});
    args.addOption({"markdown", "Start in full-document Markdown mode"});
    args.addOption({"overview", "Start in slide overview mode"});
    args.addOption({"screenshot", "Save editor screenshot and exit", "file"});
    QCommandLineOption snapshotOption("export-snapshot", "Internal export snapshot", "file");
    snapshotOption.setFlags(QCommandLineOption::HiddenFromHelp);
    args.addOption(snapshotOption);
    if (!command && !args.parse(arguments)) {
        fprintf(stderr, "%s\n", qPrintable(args.errorText())); return 1;
    }
    const bool help = !command && (args.isSet("help") || args.isSet("help-all"));
    const bool version = !command && args.isSet("version");
    const bool exporting = !command && (args.isSet("pdf") || args.isSet("pptx") || args.isSet("render"));
    const bool windowless = command || help || version || exporting || (!command && args.isSet(snapshotOption));
    if (!command && !help && !version) {
        QString error;
        if (args.positionalArguments().size() > 1) error = "Name only one presentation.";
        if ((int(args.isSet("home")) + int(args.isSet("new")) + int(args.isSet("open-dialog")) > 1) ||
            ((args.isSet("home") || args.isSet("new") || args.isSet("open-dialog")) && (!args.positionalArguments().isEmpty() || explicitResume || exporting || args.isSet(snapshotOption))))
            error = "Choose only one of --home, --new, open, or a presentation file.";
        bool slideOk = true;
        if (args.isSet("slide") && (args.value("slide").toInt(&slideOk) < 1 || !slideOk)) error = "--slide requires a positive slide number.";
        if ((args.isSet("home") || args.isSet("open-dialog")) && (args.isSet("theme") || args.isSet("save") || args.isSet("slide") || args.isSet("markdown") || args.isSet("overview")))
            error = "Editor options need a presentation; --home only opens Home.";
        if (args.isSet("markdown") && args.isSet("overview")) error = "Choose only one of --markdown or --overview.";
        if (!error.isEmpty()) { fprintf(stderr, "%s\n", qPrintable(error)); return 1; }
    }
    qputenv("QT_QPA_PLATFORMTHEME", "generic");
    if (windowless) qputenv("QT_QPA_PLATFORM", "offscreen");
    else if (qEnvironmentVariableIsEmpty("DISPLAY") && qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY") &&
             qEnvironmentVariable("QT_QPA_PLATFORM") != "offscreen" && qEnvironmentVariable("QT_QPA_PLATFORM") != "minimal") {
        fprintf(stderr, "Hype needs a graphical display. Start it from your desktop, or use 'hype help' for headless commands.\n"); return 1;
    }
    QGuiApplication app(argc, argv);
    app.setApplicationName("hype");
    app.setApplicationVersion("0.4.3");
    app.setDesktopFileName(qEnvironmentVariable("HYPE_DESKTOP_FILE", "hype"));
    if (command) return runCli(app.arguments());
    if (help) { fprintf(stdout, "%s", qPrintable(args.helpText())); return 0; }
    if (version) { fprintf(stdout, "hype %s\n", qPrintable(app.applicationVersion())); return 0; }
    const bool capture = args.isSet("screenshot");
    QLocalServer instance;
    std::unique_ptr<QLockFile> instanceLock;
    QJsonObject launch{{"action", args.isSet("new") ? "new" : args.isSet("open-dialog") ? "dialog" : args.isSet("home") ? "home" : !args.positionalArguments().isEmpty() ? "file" : explicitResume ? "continue" : "default"}};
    if (!args.positionalArguments().isEmpty()) launch["path"] = QFileInfo(args.positionalArguments().first()).absoluteFilePath();
    for (const auto &option : {"slide", "theme"}) if (args.isSet(option)) launch[option] = args.value(option);
    for (const auto &option : {"markdown", "overview", "save"}) launch[option] = args.isSet(option);
    if (!windowless && !capture) {
        const QString profile = QStandardPaths::writableLocation(QStandardPaths::StateLocation) + '\n' +
            QStandardPaths::writableLocation(QStandardPaths::ConfigLocation);
        const QString identity = "hype-" + QString::fromLatin1(QCryptographicHash::hash(profile.toUtf8(), QCryptographicHash::Sha256).toHex().left(24));
        const QString runtime = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
        if (!QDir().mkpath(runtime)) { fprintf(stderr, "Could not prepare Hype's runtime directory.\n"); return 1; }
        instanceLock = std::make_unique<QLockFile>(runtime + '/' + identity + ".lock");
        instanceLock->setStaleLockTime(0);
        if (!instanceLock->tryLock()) {
            QLocalSocket socket;
            socket.connectToServer(runtime + '/' + identity);
            if (!socket.waitForConnected(1500)) { fprintf(stderr, "Another Hype instance is starting or unavailable. Try again shortly.\n"); return 1; }
            const auto bytes = QJsonDocument(launch).toJson(QJsonDocument::Compact) + '\n';
            if (bytes.size() > 16384 || socket.write(bytes) != bytes.size() || !socket.waitForBytesWritten(1500) || !socket.waitForReadyRead(5000)) {
                fprintf(stderr, "The running Hype instance did not accept the request.\n"); return 1;
            }
            return socket.readAll().startsWith("ok") ? 0 : 1;
        }
        instance.setSocketOptions(QLocalServer::UserAccessOption);
        QLocalServer::removeServer(runtime + '/' + identity);
        if (!instance.listen(runtime + '/' + identity)) { fprintf(stderr, "Could not start Hype's local instance listener.\n"); return 1; }
    }
    Deck deck;
    const bool exportWorker = args.isSet(snapshotOption);
    auto report = [](const QJsonObject &event) {
        const auto line = QJsonDocument(event).toJson(QJsonDocument::Compact);
        fprintf(stdout, "%s\n", line.constData());
        fflush(stdout);
    };
    if (exportWorker) {
        if (!args.isSet("pdf") && !args.isSet("pptx")) return 1;
        if (!deck.loadExportSnapshot(args.value(snapshotOption))) {
            report({{"error", deck.status()}});
            return 1;
        }
        QObject::connect(&deck, &Deck::exportAdvanced, &app, [report](double progress, const QString &message) {
            report({{"progress", progress}, {"message", message}});
        });
    }
    auto positional = args.positionalArguments();
    if (exporting && positional.isEmpty() && !exportWorker) {
        fprintf(stderr, "Name a Markdown presentation to export.\n");
        return 1;
    }
    if (!windowless) deck.enableGuiSession(!capture);
    if (!positional.isEmpty() && !deck.loadPath(positional[0], !windowless && !capture)) {
        if (windowless) { fprintf(stderr, "%s\n", qPrintable(deck.status())); return 1; }
        const auto error = deck.status(); deck.showHome(); deck.setStatus("Could not open " + positional[0] + ": " + error);
    }
    if (!windowless) {
        deck.enableAutosave({}, capture);
        if (args.isSet("new")) deck.newDeck();
        else if (positional.isEmpty() && !args.isSet("home") && !args.isSet("open-dialog")) { if (explicitResume) deck.resumeLastGui(); else if (deck.continueOnStartup()) deck.continuePresentation(); }
        if (deck.home() && !args.isSet("home") && positional.isEmpty() &&
            (args.isSet("markdown") || args.isSet("overview") || args.isSet("theme") || args.isSet("slide") || args.isSet("save"))) {
            if (!deck.resumeLastGui()) {
                if (args.isSet("save")) { fprintf(stderr, "Name a presentation to save, or open one in the editor first.\n"); return 1; }
                deck.newDeck();
            }
        }
    }
    if (args.isSet("theme")) deck.chooseTheme(args.value("theme"));
    if (args.isSet("save")) deck.save();
    if (args.isSet("slide")) deck.select(args.value("slide").toInt() - 1);
    bool success = true, headless = false;
    for (const QString &option : {QString("pdf"), QString("pptx"), QString("render")})
        if (args.isSet(option)) {
            headless = true;
            success = success && (option == "pdf"    ? deck.exportPdf(args.value(option))
                                  : option == "pptx" ? deck.exportPptx(args.value(option))
                                                     : deck.renderImages(args.value(option)));
        }
    if (headless) {
        if (exportWorker) {
            if (success) report({{"progress", 1.0}, {"message", deck.status()}});
            else report({{"error", deck.status()}});
            return success ? 0 : 1;
        }
        fprintf(success ? stdout : stderr, "%s\n", qPrintable(deck.status()));
        return success ? 0 : 1;
    }
    adoptDesktopFont();
    QQuickStyle::setStyle("Basic");
    qmlRegisterType<SlideItem>("Hype", 1, 0, "SlideCanvas");
    qmlRegisterType<AppTheme>("Hype", 1, 0, "AppTheme");
    QQmlApplicationEngine engine;
    QObject::connect(&engine, &QQmlEngine::warnings, [](const QList<QQmlError> &errors) {
        for (const auto &error : errors)
            fprintf(stderr, "%s\n", qPrintable(error.toString()));
    });
    engine.rootContext()->setContextProperty("deck", &deck);
    QPointer<Thumbnails> thumbnails = new Thumbnails(&deck);
    engine.addImageProvider("slides", thumbnails);
    auto drainRenders = [thumbnails] {
        if (thumbnails)
            thumbnails->shutdown();
        // Clipboard image compression can also decode SVG through Qt GUI.
        QThreadPool::globalInstance()->waitForDone();
    };
    // The engine is not the final owner of an async image provider. Drain while
    // QGuiApplication's fonts, platform integration and GPU resources still exist.
    QObject::connect(&app, &QCoreApplication::aboutToQuit, &app, drainRenders);
    const auto renderShutdown = qScopeGuard(drainRenders);
    engine.load(QUrl("qrc:/Main.qml"));
    if (engine.rootObjects().isEmpty())
        return 1;
    QObject::connect(&instance, &QLocalServer::newConnection, &app, [&] {
        while (auto socket = instance.nextPendingConnection()) {
            QTimer::singleShot(5000, socket, &QLocalSocket::abort);
            QObject::connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
            QObject::connect(socket, &QLocalSocket::readyRead, &app, [&, socket] {
                QByteArray bytes = socket->property("request").toByteArray() + socket->readAll();
                if (bytes.size() > 16384) { socket->abort(); return; }
                if (!bytes.contains('\n')) { socket->setProperty("request", bytes); return; }
                const auto request = QJsonDocument::fromJson(bytes.left(bytes.indexOf('\n'))).object();
                const QString action = request["action"].toString();
                bool accepted = true;
                if (action == "file") accepted = deck.openPresentation(request["path"].toString());
                else if (action == "new") { deck.newDeck(); accepted = !deck.home(); }
                else if (action == "home") accepted = deck.showHome();
                else if (action == "dialog") deck.openDialog();
                else if (action == "continue") accepted = deck.resumeLastGui();
                else if (action == "default") { if (deck.home() && deck.continueOnStartup()) deck.continuePresentation(); }
                else accepted = false;
                if (accepted && deck.home() && (action == "default" || action == "continue") &&
                    (request["markdown"].toBool() || request["overview"].toBool() || request.contains("theme") || request.contains("slide") || request["save"].toBool())) {
                    if (!deck.resumeLastGui()) {
                        if (request["save"].toBool()) accepted = false;
                        else deck.newDeck();
                    }
                }
                if (accepted && !deck.home()) {
                    if (request.contains("theme")) deck.chooseTheme(request["theme"].toString());
                    if (request["save"].toBool()) deck.save();
                    if (request.contains("slide")) deck.select(request["slide"].toString().toInt() - 1);
                    if (request["markdown"].toBool() || request["overview"].toBool())
                        QMetaObject::invokeMethod(engine.rootObjects().first(), "initializeEditor", Q_ARG(QVariant, request["overview"].toBool() ? "overview" : "markdown"));
                }
                auto window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
                if (window) { window->show(); window->raise(); window->requestActivate(); }
                socket->write(accepted ? "ok\n" : "error\n"); socket->disconnectFromServer();
            });
            if (socket->bytesAvailable()) QMetaObject::invokeMethod(socket, "readyRead", Qt::QueuedConnection);
        }
    });
    if (instance.hasPendingConnections()) QMetaObject::invokeMethod(&instance, "newConnection", Qt::QueuedConnection);
    if (!deck.home()) {
        const QString mode = args.isSet("overview") ? "overview" : args.isSet("markdown") ? "markdown" : deck.editorMode();
        QMetaObject::invokeMethod(engine.rootObjects().first(), "initializeEditor", Q_ARG(QVariant, mode));
    }
    if (args.isSet("open-dialog")) QTimer::singleShot(0, &deck, &Deck::openDialog);
    if (args.isSet("screenshot")) {
        QTimer::singleShot(1800, &app, [&] {
            auto window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
            bool ok = window && window->grabWindow().save(args.value("screenshot"));
            app.exit(ok ? 0 : 1);
        });
    }
    return app.exec();
}
