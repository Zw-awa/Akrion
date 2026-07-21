#include "core/component_registry.h"
#include "core/component_pipeline.h"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>

#include <iostream>

using akrion::core::ComponentRegistry;

namespace {
int failures = 0;

void check(bool condition, const char* message) {
    if (condition) return;
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
}

QByteArray json(const QJsonObject& object) {
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

void testRegistry() {
    const ComponentRegistry registry = ComponentRegistry::withBuiltins();
    check(registry.find(QStringLiteral("step")) != nullptr, "step input is registered");
    check(registry.find(QStringLiteral("gaussian")) != nullptr, "gaussian noise is registered");
    check(registry.find(QStringLiteral("p_control")) != nullptr, "P controller is registered");
    check(registry.byKind(AKRION_COMPONENT_INPUT).size() >= 2,
          "input registry exposes multiple built-ins");
}

void testProcessLifecycle() {
    const ComponentRegistry registry = ComponentRegistry::withBuiltins();
    const auto* descriptor = registry.find(QStringLiteral("p_control"));
    check(descriptor != nullptr, "P controller descriptor exists");
    if (!descriptor) return;

    const QByteArray parameters = json({{QStringLiteral("kp"), 2.0}});
    char error[128] = {};
    void* instance = descriptor->create(reinterpret_cast<const uint8_t*>(parameters.constData()),
                                        static_cast<size_t>(parameters.size()), error, sizeof(error));
    check(instance != nullptr, "component creates a state instance");
    if (!instance) return;

    const akrion_value_v1 values[] = {{1, 1.0}, {2, 0.75}};
    akrion_input_v1 input{};
    input.device_time_us = 1000;
    input.algorithm_tick = 1;
    input.emit_tick = 1;
    input.sequence = 1;
    input.dt_seconds = 0.001;
    input.values = values;
    input.value_count = 2;
    akrion_value_v1 outputValues[4] = {};
    akrion_output_v1 output{outputValues, 0, 4};
    const int status = descriptor->process(instance, &input, &output);
    check(status == AKRION_COMPONENT_OK, "component processes a structured input");
    check(output.value_count == 2 && output.values[0].channel_id == 3
              && output.values[0].value > 0.49,
          "component emits declared control and error values");
    descriptor->destroy(instance);
}

void testRegistryRejectsDuplicate() {
    ComponentRegistry registry;
    const auto* descriptor = ComponentRegistry::withBuiltins().find(QStringLiteral("step"));
    QString error;
    check(registry.registerComponent(descriptor, &error), "first registration succeeds");
    check(!registry.registerComponent(descriptor, &error)
              && error.contains(QStringLiteral("duplicate")),
          "duplicate component IDs are rejected");
}

void testPipelineUsesSeparatePeriods() {
    akrion::core::RunConfig config;
    config.timing.algorithmPeriodUs = 1000;
    config.timing.emitPeriodUs = 10000;
    config.scenario = {
        {QStringLiteral("input"), QJsonObject{{QStringLiteral("component"), QStringLiteral("step")},
                                               {QStringLiteral("parameters"), QJsonObject{
                                                   {QStringLiteral("profile"), QStringLiteral("step")},
                                                   {QStringLiteral("step_at_s"), 0.0}}}}},
        {QStringLiteral("noise"), QJsonObject{{QStringLiteral("component"), QStringLiteral("none")},
                                               {QStringLiteral("parameters"), QJsonObject{
                                                   {QStringLiteral("profile"), QStringLiteral("none")}}}}},
        {QStringLiteral("algorithm"), QJsonObject{{QStringLiteral("component"), QStringLiteral("p_control")},
                                                   {QStringLiteral("parameters"), QJsonObject{
                                                       {QStringLiteral("kp"), 2.0}}}}},
    };
    akrion::core::AlgorithmDefinition algorithm;
    algorithm.id = 1;
    algorithm.parameters = {{QStringLiteral("kp"), 2.0}};
    config.algorithms.append(algorithm);
    akrion::core::ComponentPipeline pipeline(config);
    QString error;
    check(pipeline.open(&error), "component pipeline opens from scenario references");
    if (!pipeline.isOpen()) return;
    akrion::core::Frame first;
    akrion::core::Frame second;
    check(pipeline.nextFrame(0, &first, &error) && pipeline.nextFrame(1, &second, &error),
          "component pipeline emits frames");
    check(first.algoTick == 0 && second.algoTick == 10 && second.emitTick == 1,
          "algorithm and emit ticks remain separate");
    check(first.values.contains(QStringLiteral("target"))
              && first.values.contains(QStringLiteral("actual"))
              && first.values.contains(QStringLiteral("control")),
          "pipeline merges declared stage outputs into a frame");
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    testRegistry();
    testProcessLifecycle();
    testRegistryRejectsDuplicate();
    testPipelineUsesSeparatePeriods();
    if (failures == 0) std::cout << "All component tests passed\n";
    return failures == 0 ? 0 : 1;
}
