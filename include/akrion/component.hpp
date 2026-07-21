#pragma once

#include "component.h"

#include <cstddef>
#include <span>

namespace akrion::component {

class InputView final {
public:
    explicit InputView(const akrion_input_v1& input) : m_input(input) {}

    uint64_t deviceTimeUs() const { return m_input.device_time_us; }
    uint64_t algorithmTick() const { return m_input.algorithm_tick; }
    uint64_t emitTick() const { return m_input.emit_tick; }
    double dtSeconds() const { return m_input.dt_seconds; }

    bool find(uint32_t channelId, double* value) const {
        if (!value) return false;
        for (size_t index = 0; index < m_input.value_count; ++index) {
            if (m_input.values[index].channel_id != channelId) continue;
            *value = m_input.values[index].value;
            return true;
        }
        return false;
    }

    std::span<const akrion_value_v1> values() const {
        return {m_input.values, m_input.value_count};
    }

private:
    const akrion_input_v1& m_input;
};

class OutputWriter final {
public:
    explicit OutputWriter(akrion_output_v1& output) : m_output(output) {}

    bool write(uint32_t channelId, double value) {
        if (m_output.value_count >= m_output.value_capacity) return false;
        m_output.values[m_output.value_count++] = {channelId, value};
        return true;
    }

private:
    akrion_output_v1& m_output;
};

} // namespace akrion::component
