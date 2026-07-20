#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QTimer>

#include "gui/appcontroller.h"
#include "gui/waveformcontroller.h"
#include "gui/waveformview.h"

int main(int argc, char* argv[]) {
    QGuiApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("Akrion GUI"));
    app.setOrganizationName(QStringLiteral("Akrion"));
    QQuickStyle::setStyle(QStringLiteral("Basic"));

    qmlRegisterType<akrion::gui::WaveformController>("Akrion", 1, 0,
                                                     "WaveformController");
    qmlRegisterType<akrion::gui::WaveformView>("Akrion", 1, 0, "WaveformView");

    akrion::gui::AppController controller;
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("appController"), &controller);
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed, &app,
                     [] { QCoreApplication::exit(-1); }, Qt::QueuedConnection);
#if QT_AGENT_SAFE_BUILD
    engine.load(QUrl(QStringLiteral("qrc:/qml/Main.qml")));
#else
    engine.loadFromModule("Akrion", "Main");
#endif
    if (engine.rootObjects().isEmpty()) return -1;
    const auto screenshotIndex = app.arguments().indexOf(QStringLiteral("--screenshot"));
    if (screenshotIndex >= 0 && screenshotIndex + 1 < app.arguments().size()) {
        const auto screenshotPath = app.arguments().at(screenshotIndex + 1);
        controller.startDemo();
        QTimer::singleShot(1200, &app, [&app, &engine, screenshotPath] {
            const auto window = qobject_cast<QQuickWindow*>(engine.rootObjects().constFirst());
            QCoreApplication::exit(window && window->grabWindow().save(screenshotPath) ? 0 : 2);
        });
    } else if (app.arguments().contains(QStringLiteral("--smoke-test"))) {
        controller.startDemo();
        QTimer::singleShot(1500, &app, &QCoreApplication::quit);
    }
    return app.exec();
}
