#pragma once

#include <akrion/component.h>

#include <QVector>

namespace akrion::core {
QVector<const akrion_component_descriptor_v1*> builtinComponentDescriptors();
} // namespace akrion::core
