#pragma once

#include <akrion/component.h>

#include <QString>
#include <QVector>

namespace akrion::core {

class ComponentRegistry final {
public:
    bool registerComponent(const akrion_component_descriptor_v1* descriptor,
                           QString* error = nullptr);
    const akrion_component_descriptor_v1* find(const QString& id) const;
    QVector<const akrion_component_descriptor_v1*> all() const;
    QVector<const akrion_component_descriptor_v1*> byKind(uint32_t kind) const;
    static ComponentRegistry withBuiltins();

private:
    QVector<const akrion_component_descriptor_v1*> m_components;
};

} // namespace akrion::core
