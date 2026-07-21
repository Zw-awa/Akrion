#include "gui/waveformcontroller.h"

#include <QCoreApplication>
#include <QVariantMap>

#include <cmath>
#include <iostream>

namespace {

using akrion::gui::WaveformController;
int failures = 0;

void check(bool condition, const char* message) {
    if (condition) return;
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
}

QVariantMap firstGroup(const WaveformController& controller) {
    const QVariantList groups = controller.groupInfo();
    return groups.isEmpty() ? QVariantMap() : groups.constFirst().toMap();
}

void append(WaveformController& controller, qint64 timeUs, quint64 sequence, double value) {
    controller.appendFrameValues(timeUs, timeUs, sequence, 1, true,
                                 {QStringLiteral("signal")}, {value});
}

void testStableFollowRange() {
    WaveformController controller;
    controller.defineChannel(QStringLiteral("signal"), QStringLiteral("Signal"),
                             QStringLiteral("V"), QStringLiteral("measurement"),
                             QString(), QStringLiteral("Signal"));
    append(controller, 0, 0, -1.0);
    append(controller, 1000000, 1, 1.0);
    const QVariantMap initial = firstGroup(controller);
    const double initialMinimum = initial.value(QStringLiteral("minimum")).toDouble();
    const double initialMaximum = initial.value(QStringLiteral("maximum")).toDouble();
    const qint64 initialViewEnd = controller.viewEndUs();

    append(controller, 20000000, 2, 0.2);
    const QVariantMap shifted = firstGroup(controller);
    check(controller.viewEndUs() > initialViewEnd, "follow advances the horizontal viewport");
    check(std::abs(shifted.value(QStringLiteral("minimum")).toDouble() - initialMinimum) < 1e-12
              && std::abs(shifted.value(QStringLiteral("maximum")).toDouble() - initialMaximum) < 1e-12,
          "follow does not rescale Y when old extrema leave the viewport");

    append(controller, 21000000, 3, 3.0);
    const QVariantMap expanded = firstGroup(controller);
    check(expanded.value(QStringLiteral("maximum")).toDouble() > initialMaximum,
          "automatic Y range expands for a new extreme");
    check(std::abs(expanded.value(QStringLiteral("minimum")).toDouble() - initialMinimum) < 1e-12,
          "automatic Y range does not contract its opposite edge");
}

void testManualVerticalZoom() {
    WaveformController controller;
    controller.defineChannel(QStringLiteral("signal"), QStringLiteral("Signal"), QString(),
                             QStringLiteral("measurement"), QString(), QStringLiteral("Signal"));
    append(controller, 0, 0, -2.0);
    append(controller, 1000, 1, 2.0);
    const QVariantMap before = firstGroup(controller);
    const double oldSpan = before.value(QStringLiteral("maximum")).toDouble()
        - before.value(QStringLiteral("minimum")).toDouble();
    controller.zoomGroupAt(QStringLiteral("Signal"), 0.5, 2.0);
    const QVariantMap zoomed = firstGroup(controller);
    const double newSpan = zoomed.value(QStringLiteral("maximum")).toDouble()
        - zoomed.value(QStringLiteral("minimum")).toDouble();
    check(std::abs(newSpan - oldSpan / 2.0) < oldSpan * 1e-9,
          "vertical wheel zoom changes only the selected Y range");
    check(!zoomed.value(QStringLiteral("autoRange")).toBool(),
          "manual vertical zoom disables automatic expansion");

    controller.panGroupBy(QStringLiteral("Signal"), 0.25);
    const QVariantMap panned = firstGroup(controller);
    check(std::abs((panned.value(QStringLiteral("maximum")).toDouble()
                   - panned.value(QStringLiteral("minimum")).toDouble()) - newSpan) < 1e-12
              && panned.value(QStringLiteral("minimum")).toDouble()
                     > zoomed.value(QStringLiteral("minimum")).toDouble(),
          "vertical drag translates Y without changing its span");

    append(controller, 2000, 2, 20.0);
    const QVariantMap fixed = firstGroup(controller);
    check(std::abs(fixed.value(QStringLiteral("maximum")).toDouble()
                   - panned.value(QStringLiteral("maximum")).toDouble()) < 1e-12,
          "manual Y range stays fixed while new frames arrive");
    controller.resetGroupRange(QStringLiteral("Signal"));
    const QVariantMap fitted = firstGroup(controller);
    check(fitted.value(QStringLiteral("autoRange")).toBool()
              && fitted.value(QStringLiteral("maximum")).toDouble() > 20.0,
          "fit Y restores automatic expansion and includes recorded extrema");
}

void testNiceTicks() {
    const QVector<double> ticks = WaveformController::numericTicks(-1.2, 8.8, 6);
    check(ticks.size() >= 4 && ticks.size() <= 7, "nice ticks stay near the requested density");
    check(ticks.contains(0.0), "nice ticks include zero when the range crosses zero");
    if (ticks.size() < 2) return;
    const double step = ticks[1] - ticks[0];
    const double magnitude = std::pow(10.0, std::floor(std::log10(step)));
    const double normalized = step / magnitude;
    check(std::abs(normalized - 1.0) < 1e-9 || std::abs(normalized - 2.0) < 1e-9
              || std::abs(normalized - 5.0) < 1e-9 || std::abs(normalized - 10.0) < 1e-9,
          "tick spacing follows the 1/2/5 progression");
}

void testChannelPresentationAndReset() {
    WaveformController controller;
    controller.defineChannel(QStringLiteral("first"), QStringLiteral("First"), QString(),
                             QStringLiteral("other"), QString(), QStringLiteral("Signals"));
    controller.defineChannel(QStringLiteral("second"), QStringLiteral("Second"), QString(),
                             QStringLiteral("other"), QString(), QStringLiteral("Signals"));
    controller.setChannelLabel(QStringLiteral("first"), QStringLiteral("Renamed"));
    controller.setChannelColor(QStringLiteral("first"), QStringLiteral("#e83e8c"));
    const auto channels = controller.channelDefinitions();
    check(controller.channelCount() == 2 && channels.first().label == QStringLiteral("Renamed"),
          "channel display name is editable without changing its data key");
    check(channels.first().color == QColor(QStringLiteral("#e83e8c")),
          "channel color is editable");
    controller.resetChannels();
    check(controller.channelCount() == 0,
          "resetting a data source removes channels that are no longer present");
}

void testPausedCursorLookup() {
    WaveformController controller;
    controller.defineChannel(QStringLiteral("signal"), QStringLiteral("Signal"), QString(),
                             QStringLiteral("measurement"), QString(), QStringLiteral("Signal"));
    append(controller, 0, 0, 0.0);
    append(controller, 1000, 1, 1.0);
    controller.setPaused(true);
    controller.setCursorAt(1.0, QStringLiteral("Signal"), 0.5, 400.0, 200.0);
    check(controller.cursorVisible() && !controller.cursorValues().isEmpty(),
          "cursor lookup remains available while rendering is paused");
}

void testCrossingSnap() {
    WaveformController controller;
    controller.defineChannel(QStringLiteral("first"), QStringLiteral("First"), QString(),
                             QStringLiteral("measurement"), QString(), QStringLiteral("Signals"));
    controller.defineChannel(QStringLiteral("second"), QStringLiteral("Second"), QString(),
                             QStringLiteral("measurement"), QString(), QStringLiteral("Signals"));
    controller.appendFrameValues(0, 0, 0, 1, true,
                                 {QStringLiteral("first"), QStringLiteral("second")},
                                 {-1.0, 1.0});
    controller.appendFrameValues(1000, 1000, 1, 1, true,
                                 {QStringLiteral("first"), QStringLiteral("second")},
                                 {1.0, -1.0});
    const double normalizedCrossing = static_cast<double>(500 - controller.viewStartUs())
        / static_cast<double>(controller.viewEndUs() - controller.viewStartUs());
    const QVariantMap group = firstGroup(controller);
    const double minimum = group.value(QStringLiteral("minimum")).toDouble();
    const double maximum = group.value(QStringLiteral("maximum")).toDouble();
    const double crossingY = maximum / (maximum - minimum);
    controller.setCursorAt(normalizedCrossing, QStringLiteral("Signals"), crossingY, 800.0, 300.0);
    check(controller.cursorSnapped() && controller.cursorSnapLabel().contains(QStringLiteral("交叉点")),
          "cursor prioritizes an interpolated crossing inside the snap radius");
}

} // namespace

int main(int argc, char* argv[]) {
    QCoreApplication application(argc, argv);
    testStableFollowRange();
    testManualVerticalZoom();
    testNiceTicks();
    testChannelPresentationAndReset();
    testPausedCursorLookup();
    testCrossingSnap();
    if (failures == 0) std::cout << "All waveform tests passed\n";
    return failures == 0 ? 0 : 1;
}
