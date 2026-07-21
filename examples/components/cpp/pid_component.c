#include <akrion/component.h>

#include <stdlib.h>

typedef struct {
    double kp;
} pid_state;

static void* pid_create(const uint8_t* parameters, size_t size,
                        char* error, size_t error_capacity) {
    (void)parameters;
    (void)size;
    (void)error;
    (void)error_capacity;
    pid_state* state = (pid_state*)calloc(1, sizeof(pid_state));
    if (state) state->kp = 1.2;
    return state;
}

static int pid_process(void* instance, const akrion_input_v1* input,
                       akrion_output_v1* output) {
    pid_state* state = (pid_state*)instance;
    double target = 0.0;
    double actual = 0.0;
    if (!state || !input || !output || output->value_capacity < 2) {
        return AKRION_COMPONENT_INVALID_ARGUMENT;
    }
    for (size_t index = 0; index < input->value_count; ++index) {
        if (input->values[index].channel_id == 1) target = input->values[index].value;
        if (input->values[index].channel_id == 2) actual = input->values[index].value;
    }
    output->values[0] = (akrion_value_v1){3, state->kp * (target - actual)};
    output->values[1] = (akrion_value_v1){6, target - actual};
    output->value_count = 2;
    return AKRION_COMPONENT_OK;
}

static void pid_destroy(void* instance) { free(instance); }

static const akrion_channel_descriptor_v1 pid_inputs[] = {
    {1, "target", "Target", "", "reference"},
    {2, "actual", "Actual", "", "measurement"},
};
static const akrion_channel_descriptor_v1 pid_outputs[] = {
    {3, "control", "Control", "", "control"},
    {6, "error", "Error", "", "diagnostic"},
};

const akrion_component_descriptor_v1 akrion_example_pid = {
    AKRION_COMPONENT_ABI_V1,
    AKRION_COMPONENT_ALGORITHM,
    "example/c_pid",
    "Example C PID",
    "0.1.0",
    "{}",
    pid_inputs,
    2,
    pid_outputs,
    2,
    pid_create,
    pid_process,
    pid_destroy,
};
