#ifndef AKRION_COMPONENT_H
#define AKRION_COMPONENT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AKRION_COMPONENT_ABI_V1 1u

typedef enum akrion_component_kind_v1 {
    AKRION_COMPONENT_INPUT = 1u,
    AKRION_COMPONENT_NOISE = 2u,
    AKRION_COMPONENT_ALGORITHM = 3u
} akrion_component_kind_v1;

typedef enum akrion_component_status_v1 {
    AKRION_COMPONENT_OK = 0,
    AKRION_COMPONENT_INVALID_ARGUMENT = 1,
    AKRION_COMPONENT_INVALID_PARAMETERS = 2,
    AKRION_COMPONENT_OUTPUT_FULL = 3,
    AKRION_COMPONENT_MISSING_INPUT = 4,
    AKRION_COMPONENT_RUNTIME_ERROR = 5
} akrion_component_status_v1;

typedef struct akrion_channel_descriptor_v1 {
    uint32_t id;
    const char* key;
    const char* label;
    const char* unit;
    const char* role;
} akrion_channel_descriptor_v1;

typedef struct akrion_value_v1 {
    uint32_t channel_id;
    double value;
} akrion_value_v1;

typedef struct akrion_input_v1 {
    uint64_t device_time_us;
    uint64_t host_time_us;
    uint64_t algorithm_tick;
    uint64_t emit_tick;
    uint64_t sequence;
    double dt_seconds;
    uint32_t flags;
    const akrion_value_v1* values;
    size_t value_count;
} akrion_input_v1;

typedef struct akrion_output_v1 {
    akrion_value_v1* values;
    size_t value_count;
    size_t value_capacity;
} akrion_output_v1;

typedef void* (*akrion_component_create_v1)(
    const uint8_t* parameters,
    size_t parameters_size,
    char* error,
    size_t error_capacity);
typedef int (*akrion_component_process_v1)(
    void* instance,
    const akrion_input_v1* input,
    akrion_output_v1* output);
typedef void (*akrion_component_destroy_v1)(void* instance);

typedef struct akrion_component_descriptor_v1 {
    uint32_t abi_version;
    uint32_t kind;
    const char* id;
    const char* name;
    const char* version;
    const char* parameter_schema_json;
    const akrion_channel_descriptor_v1* inputs;
    size_t input_count;
    const akrion_channel_descriptor_v1* outputs;
    size_t output_count;
    akrion_component_create_v1 create;
    akrion_component_process_v1 process;
    akrion_component_destroy_v1 destroy;
} akrion_component_descriptor_v1;

#ifdef __cplusplus
}
#endif

#endif
