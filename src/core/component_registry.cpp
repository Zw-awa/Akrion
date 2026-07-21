#include "component_registry.h"

#include "builtin_components.h"

namespace akrion::core {

bool ComponentRegistry::registerComponent(
    const akrion_component_descriptor_v1* descriptor, QString* error) {
    if (!descriptor || !descriptor->id || !descriptor->process || !descriptor->create
        || !descriptor->destroy || descriptor->abi_version != AKRION_COMPONENT_ABI_V1) {
        if (error) *error = QStringLiteral("invalid or unsupported component descriptor");
        return false;
    }
    const QString id = QString::fromUtf8(descriptor->id).trimmed();
    if (id.isEmpty()) {
        if (error) *error = QStringLiteral("component id is empty");
        return false;
    }
    if (find(id)) {
        if (error) *error = QStringLiteral("duplicate component id '%1'").arg(id);
        return false;
    }
    m_components.append(descriptor);
    return true;
}

const akrion_component_descriptor_v1* ComponentRegistry::find(const QString& id) const {
    for (const auto* descriptor : m_components) {
        if (QString::fromUtf8(descriptor->id) == id) return descriptor;
    }
    return nullptr;
}

QVector<const akrion_component_descriptor_v1*> ComponentRegistry::all() const {
    return m_components;
}

QVector<const akrion_component_descriptor_v1*> ComponentRegistry::byKind(uint32_t kind) const {
    QVector<const akrion_component_descriptor_v1*> result;
    for (const auto* descriptor : m_components) {
        if (descriptor->kind == kind) result.append(descriptor);
    }
    return result;
}

ComponentRegistry ComponentRegistry::withBuiltins() {
    ComponentRegistry registry;
    for (const auto* descriptor : builtinComponentDescriptors()) {
        QString error;
        if (!registry.registerComponent(descriptor, &error)) return {};
    }
    return registry;
}

} // namespace akrion::core
