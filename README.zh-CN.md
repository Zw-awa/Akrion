# Akrion

[English](./README.md) | [简体中文](./README.zh-CN.md)

[![License](https://img.shields.io/badge/license-Apache--2.0-1976d2)](./LICENSE)
![Focus](https://img.shields.io/badge/focus-real--device%20algorithm%20evaluation-2e7d32)
![Scope](https://img.shields.io/badge/scope-structured%20recording%20%2B%20replay%20%2B%20comparison-f57c00)

Akrion 是一个面向真机算法调试的实验记录与测评框架，服务于控制、滤波和估计算法。

它解决的是一个很具体的问题：算法在纸面上可行，不代表在真实设备上就真的好用。Akrion 关注的是把真实输入、真实扰动、真实输出，以及实验时使用的算法和参数，结构化地记录下来，并在之后进行可重复的对比。

## 目录

- [Akrion](#akrion)
  - [目录](#目录)
  - [Akrion 是什么](#akrion-是什么)
  - [它为什么存在](#它为什么存在)
  - [用户能得到什么](#用户能得到什么)
  - [适合的使用场景](#适合的使用场景)
  - [核心工作流](#核心工作流)
  - [设计原则](#设计原则)
  - [快速开始](#快速开始)
  - [当前状态](#当前状态)
  - [许可证](#许可证)

## Akrion 是什么

Akrion 聚焦一个在工程里很常见的问题：

- 你实现了一个算法
- 理论上它是成立的
- 真机上也已经跑起来了
- 但你仍然无法客观判断它在当前条件下到底表现如何

Akrion 就是为这条证据链服务的。

项目围绕下面几件事展开：

- 面向真实设备的结构化串口录制
- 可复现的实验 run 保存
- 可插拔的输入、噪声与算法定义
- 理论/期望输出与真实输出的时间对齐对比
- 波形图与基础测评结果输出

## 它为什么存在

很多开发者能写出 PID、卡尔曼、中值滤波、ADRC、LQR，但仍然回答不了真正重要的问题：

“在相同条件下，这个算法在目标设备上到底表现得好不好？”

真正缺失的通常不是算法公式，而是实验基础设施：

- 没有统一的录制格式
- 没有明确区分算法运行周期和串口发送周期
- 没有可复用的实验元数据
- 没有稳定的重复实验记录
- 没有办法把真实数据保存下来，供后续算法版本回放对比

Akrion 的目的，就是把这条流程规范化。

## 用户能得到什么

Akrion 预期给用户提供这些结果：

- 用结构化格式记录真机串口输入输出
- 保存一次实验所使用的算法、参数与运行配置
- 支持用户自定义输入、干扰和算法实现
- 用同一批真实数据回放并比较后续实现
- 在不引入重型建模平台的前提下，产出波形和基础指标

## 适合的使用场景

Akrion 适合这些场景：

- 在同一个电机速度环上比较两组 PID 参数
- 在固件定时改变后验证卡尔曼滤波是否仍然稳定
- 录制一次真实扰动并在后续版本里反复回放
- 帮助初学者理解控制周期、上报周期和时间同步对结论的影响
- 保存实验历史，避免重复测试和不可追溯的结论

## 核心工作流

一次典型的 Akrion 使用流程大致是：

1. 定义或选择输入、干扰和算法
2. 在真机或可回放路径上执行实验
3. 录制结构化串口数据和实验元数据
4. 保存结果文件供后续对比
5. 查看波形和基础指标
6. 在相近条件下替换算法或参数重新运行

## 设计原则

Akrion 的设计选择是明确的：

- 真机优先，而不是仿真优先
- 结构化实验记录，而不是零散截图和口头结论
- 用户流程尽量简单，同时保留扩展能力
- 数据和元数据可复用，而不是一次性调试
- 边界保持收敛，不扩张成大而全平台

它并不打算替代 MATLAB、Simulink、通用波形工具或完整 HIL 基础设施。

## 快速开始

```bash
git clone https://github.com/Zw-awa/Akrion.git
cd Akrion
```

构建 CLI 和 GUI，检查环境，并录制一个可复现的演示运行：

```powershell
.\tools\qt.ps1 build
.\tools\qt.ps1 cli doctor --json
.\tools\qt.ps1 cli init akrion.config.json
.\tools\qt.ps1 cli demo --config akrion.config.json --duration 10
.\tools\qt.ps1 run
```

`akrion` 是采集和运行管理 CLI，`akrion-gui` 是实时波形与回放程序。

## 当前状态

当前版本已经拆分为强类型 C++ 共享核心、完整录制 CLI 和 Qt/QML 桌面波形程序。

CLI 包含：

- `doctor`、`ports`、`init`、`record`、`demo`
- `validate`、`runs list`、`runs show`、`replay`
- `export`、`pack`、`unpack`
- 机器可读的 `--json` 输出和明确的退出码

GUI 支持有界实时缓冲、像素级最小值/最大值降采样、设备/主机时间轴、暂停与跟随、缩放、平移、游标、通道分组、算法停用区间、事件标记和单次运行回放。

### 构建与运行

需要 Qt 6.5 或更高版本，并安装 `Core`、`Gui`、`Qml`、`Quick`、`QuickControls2`、`SerialPort` 组件。

```powershell
.\tools\qt.ps1 build
.\tools\qt.ps1 test
.\tools\qt.ps1 smoke
.\tools\qt.ps1 run
.\tools\qt.ps1 cli runs list --json
```

统一入口按“命令行参数、环境变量、`.env`、自动发现”的顺序定位 Qt，并固定使用该安装中的 CMake、MinGW 和 `MinGW Makefiles`。需要本地配置时，将 `.env.example` 复制为 `.env` 并填写 `QT_ROOT`、`QT_VERSION`；`.env` 不会被 Git 提交。检测到 Codex 时自动启用 agent-safe CMake 分支，普通终端仍使用标准 `qt_add_qml_module`。

### 串口协议

每行发送一个 JSON 对象并以换行结束，完整约束见 [`schemas/serial-frame.schema.json`](./schemas/serial-frame.schema.json)。

```json
{"schema":"akrion.frame/1","device_time_us":20000,"algo_tick":20,"emit_tick":1,"seq":1,"algo_id":1,"algo_enabled":true,"values":{"target":1.0,"actual":0.82,"control":0.31}}
```

`algo_tick` 是算法执行计数，`emit_tick` 是串口发送计数；两者必须分开记录。

每次采集都会流式写入可恢复的运行目录：

```text
<run-id>/
  manifest.json
  config.json
  serial.raw
  frames.ndjson
  events.ndjson
  summary.json
```

`serial.raw` 原样保存收到的全部字节。结束后的运行可以导出 CSV/NDJSON，也可以打包为标准 `.akrion` ZIP 容器。

Akrion 只输出事实性的传输、时序、通道统计，以及显式映射下的 MAE/RMSE；不会自动推荐算法，也不会推断算法是否“好用”。

## 许可证

Akrion 使用 Apache License 2.0。见 [LICENSE](./LICENSE)。

