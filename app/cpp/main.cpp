// ResearchSSH-Next — application entry point.
//
// Wires the Qt Quick engine to the AppController. The controller is exposed to
// QML as the context property `app`; QML reads its models/properties and calls
// its slots, and never touches the FFI or any secret directly.

#include <QFont>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QtGlobal>

#include "AppController.h"

int main(int argc, char *argv[]) {
    QGuiApplication app(argc, argv);
    QGuiApplication::setApplicationName(QStringLiteral("ResearchSSH-Next"));
    QGuiApplication::setOrganizationName(QStringLiteral("ResearchSSH"));

    // Base UI font: a clean CJK-capable family so Chinese labels render nicely.
    // (The terminal pane overrides this with a monospace family.)
    QFont uiFont;
    uiFont.setFamilies({QStringLiteral("Microsoft YaHei UI"), QStringLiteral("Microsoft YaHei"),
                        QStringLiteral("Segoe UI")});
    uiFont.setPixelSize(13);
    QGuiApplication::setFont(uiFont);

    researchssh::AppController controller;
    QString initError;
    if (!controller.initialize(&initError)) {
        qCritical("Failed to initialize Rust core: %s", qPrintable(initError));
        return 1;
    }

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("app"), &controller);

    QObject::connect(
        &engine, &QQmlApplicationEngine::objectCreationFailed, &app,
        []() { QCoreApplication::exit(2); }, Qt::QueuedConnection);

    // Loads qml/Main.qml registered under the "ResearchSSH" module (Qt 6.5+).
    engine.loadFromModule("ResearchSSH", "Main");
    if (engine.rootObjects().isEmpty())
        return 2;

    return app.exec();
}
