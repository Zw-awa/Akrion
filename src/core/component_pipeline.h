#pragma once

#include "component_registry.h"
#include "types.h"

#include <QHash>
#include <QJsonObject>

#include <memory>

namespace akrion::core {

class ComponentPipeline final {
public:
    explicit ComponentPipeline(RunConfig config,
                               ComponentRegistry registry = ComponentRegistry::withBuiltins());
    ~ComponentPipeline();

    ComponentPipeline(const ComponentPipeline&) = delete;
    ComponentPipeline& operator=(const ComponentPipeline&) = delete;

    bool open(QString* error = nullptr);
    void close();
    bool isOpen() const { return m_open; }
    bool nextFrame(quint64 emitTick, Frame* frame, QString* error = nullptr);

private:
    struct Instance {
        const akrion_component_descriptor_v1* descriptor = nullptr;
        void* state = nullptr;
    };

    bool createInstance(uint32_t kind, const QJsonObject& reference,
                        const QString& fallbackId, Instance* instance, QString* error);
    bool process(Instance& instance, quint64 deviceTimeUs, quint64 algorithmTick,
                 quint64 emitTick, quint64 sequence, QString* error);
    void mergeOutputs(const Instance& instance, const QVector<akrion_value_v1>& outputs);
    QJsonObject componentReference(const QString& key) const;

    RunConfig m_config;
    ComponentRegistry m_registry;
    Instance m_input;
    Instance m_noise;
    Instance m_algorithm;
    bool m_algorithmEnabled = true;
    bool m_open = false;
    quint64 m_nextAlgorithmTimeUs = 0;
    quint64 m_algorithmTick = 0;
    QHash<uint32_t, double> m_values;
    QHash<uint32_t, QString> m_channelKeys;
};

} // namespace akrion::core
