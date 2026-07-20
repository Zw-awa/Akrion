#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>

// Send one JSON object followed by '\n' at the reporting period. The algorithm
// may run many times between calls, so algo_tick and emit_tick stay separate.
void akrion_emit(uint64_t device_time_us,
                 uint64_t algo_tick,
                 uint64_t emit_tick,
                 uint64_t seq,
                 uint32_t algorithm_id,
                 int algorithm_enabled,
                 float reference,
                 float measurement,
                 float control) {
    printf("{\"schema\":\"akrion.frame/1\",\"device_time_us\":%" PRIu64
           ",\"algo_tick\":%" PRIu64 ",\"emit_tick\":%" PRIu64
           ",\"seq\":%" PRIu64 ",\"algo_id\":%" PRIu32
           ",\"algo_enabled\":%s,\"values\":{\"reference\":%.6f,"
           "\"measurement\":%.6f,\"control\":%.6f}}\n",
           device_time_us,
           algo_tick,
           emit_tick,
           seq,
           algorithm_id,
           algorithm_enabled ? "true" : "false",
           reference,
           measurement,
           control);
}
