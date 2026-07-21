#include <akrion/component.h>

int main(void) {
    akrion_value_v1 values[2] = {{1u, 1.0}, {2u, 0.5}};
    akrion_input_v1 input = {0};
    input.values = values;
    input.value_count = 2;
    akrion_value_v1 output_values[2] = {{0u, 0.0}, {0u, 0.0}};
    akrion_output_v1 output = {output_values, 0, 2};
    return input.value_count == 2 && output.value_capacity == 2 ? 0 : 1;
}
