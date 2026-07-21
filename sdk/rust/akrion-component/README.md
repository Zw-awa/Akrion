# Akrion Rust Component SDK

This crate mirrors the stable C ABI used by Akrion components. A component receives a
pointer-and-length input view and writes declared `channel_id/value` outputs into a host-owned
buffer. Component state is allocated and released by the component library itself.

The exported `akrion_component_entry_v1` symbol is suitable for a future shared-library loader;
the current Akrion release uses static registration while the ABI is stabilized.

```powershell
cargo test --manifest-path sdk/rust/akrion-component/Cargo.toml
```
