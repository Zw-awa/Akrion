use std::ffi::c_void;

pub const ABI_V1: u32 = 1;
pub const COMPONENT_ALGORITHM: u32 = 3;
pub const OK: i32 = 0;
pub const OUTPUT_FULL: i32 = 3;
pub const MISSING_INPUT: i32 = 4;

#[repr(C)]
pub struct ChannelDescriptor {
    pub id: u32,
    pub key: *const u8,
    pub label: *const u8,
    pub unit: *const u8,
    pub role: *const u8,
}

unsafe impl Sync for ChannelDescriptor {}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct Value {
    pub channel_id: u32,
    pub value: f64,
}

#[repr(C)]
pub struct Input {
    pub device_time_us: u64,
    pub host_time_us: u64,
    pub algorithm_tick: u64,
    pub emit_tick: u64,
    pub sequence: u64,
    pub dt_seconds: f64,
    pub flags: u32,
    pub values: *const Value,
    pub value_count: usize,
}

#[repr(C)]
pub struct Output {
    pub values: *mut Value,
    pub value_count: usize,
    pub value_capacity: usize,
}

pub type CreateFn = unsafe extern "C" fn(*const u8, usize, *mut u8, usize) -> *mut c_void;
pub type ProcessFn = unsafe extern "C" fn(*mut c_void, *const Input, *mut Output) -> i32;
pub type DestroyFn = unsafe extern "C" fn(*mut c_void);

#[repr(C)]
pub struct Descriptor {
    pub abi_version: u32,
    pub kind: u32,
    pub id: *const u8,
    pub name: *const u8,
    pub version: *const u8,
    pub parameter_schema_json: *const u8,
    pub inputs: *const ChannelDescriptor,
    pub input_count: usize,
    pub outputs: *const ChannelDescriptor,
    pub output_count: usize,
    pub create: CreateFn,
    pub process: ProcessFn,
    pub destroy: DestroyFn,
}

unsafe impl Sync for Descriptor {}

#[derive(Default)]
struct PidState {
    kp: f64,
}

unsafe extern "C" fn create(
    _parameters: *const u8,
    _parameters_size: usize,
    _error: *mut u8,
    _error_capacity: usize,
) -> *mut c_void {
    Box::into_raw(Box::new(PidState { kp: 1.2 })) as *mut c_void
}

unsafe extern "C" fn process(instance: *mut c_void, input: *const Input, output: *mut Output) -> i32 {
    if instance.is_null() || input.is_null() || output.is_null() {
        return MISSING_INPUT;
    }
    let state = &*(instance as *const PidState);
    let input = &*input;
    let output = &mut *output;
    let values = std::slice::from_raw_parts(input.values, input.value_count);
    let mut target = None;
    let mut actual = None;
    for value in values {
        if value.channel_id == 1 {
            target = Some(value.value);
        } else if value.channel_id == 2 {
            actual = Some(value.value);
        }
    }
    let (Some(target), Some(actual)) = (target, actual) else {
        return MISSING_INPUT;
    };
    if output.value_capacity < 2 || output.values.is_null() {
        return OUTPUT_FULL;
    }
    let output_values = std::slice::from_raw_parts_mut(output.values, output.value_capacity);
    output_values[0] = Value { channel_id: 3, value: (state.kp * (target - actual)).clamp(-1.0, 1.0) };
    output_values[1] = Value { channel_id: 6, value: target - actual };
    output.value_count = 2;
    OK
}

unsafe extern "C" fn destroy(instance: *mut c_void) {
    if !instance.is_null() {
        drop(Box::from_raw(instance as *mut PidState));
    }
}

static ID: &[u8] = b"rust/pid\0";
static NAME: &[u8] = b"Rust P controller\0";
static VERSION: &[u8] = b"0.1.0\0";
static SCHEMA: &[u8] = b"{}\0";
static INPUTS: [ChannelDescriptor; 2] = [
    ChannelDescriptor { id: 1, key: b"target\0".as_ptr(), label: b"Target\0".as_ptr(), unit: b"\0".as_ptr(), role: b"reference\0".as_ptr() },
    ChannelDescriptor { id: 2, key: b"actual\0".as_ptr(), label: b"Actual\0".as_ptr(), unit: b"\0".as_ptr(), role: b"measurement\0".as_ptr() },
];
static OUTPUTS: [ChannelDescriptor; 2] = [
    ChannelDescriptor { id: 3, key: b"control\0".as_ptr(), label: b"Control\0".as_ptr(), unit: b"\0".as_ptr(), role: b"control\0".as_ptr() },
    ChannelDescriptor { id: 6, key: b"error\0".as_ptr(), label: b"Error\0".as_ptr(), unit: b"\0".as_ptr(), role: b"diagnostic\0".as_ptr() },
];

static DESCRIPTOR: Descriptor = Descriptor {
    abi_version: ABI_V1,
    kind: COMPONENT_ALGORITHM,
    id: ID.as_ptr(),
    name: NAME.as_ptr(),
    version: VERSION.as_ptr(),
    parameter_schema_json: SCHEMA.as_ptr(),
    inputs: INPUTS.as_ptr(),
    input_count: INPUTS.len(),
    outputs: OUTPUTS.as_ptr(),
    output_count: OUTPUTS.len(),
    create,
    process,
    destroy,
};

#[no_mangle]
pub extern "C" fn akrion_component_entry_v1() -> *const Descriptor {
    &DESCRIPTOR
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::ptr;

    #[test]
    fn rust_callback_emits_control_and_error() {
        let instance = unsafe { create(ptr::null(), 0, ptr::null_mut(), 0) };
        let values = [Value { channel_id: 1, value: 1.0 }, Value { channel_id: 2, value: 0.5 }];
        let input = Input {
            device_time_us: 0,
            host_time_us: 0,
            algorithm_tick: 0,
            emit_tick: 0,
            sequence: 0,
            dt_seconds: 0.001,
            flags: 0,
            values: values.as_ptr(),
            value_count: values.len(),
        };
        let mut output_values = [Value { channel_id: 0, value: 0.0 }; 2];
        let mut output = Output { values: output_values.as_mut_ptr(), value_count: 0, value_capacity: 2 };
        assert_eq!(unsafe { process(instance, &input, &mut output) }, OK);
        assert_eq!(output.value_count, 2);
        unsafe { destroy(instance) };
    }
}
