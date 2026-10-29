# E37 / HSMS

[English](./README.md) | [简体中文](./README.zh-CN.md)

基于 C++23 的异步 HSMS 传输与事务库，提供 TCP 连接、协议状态机、控制消息、数据事务、超时和重连管理。

- CMake 链接目标：`Glove::E37`。
- 入口头文件：`<HSMS/Endpoint.h>`。
- 命名空间：`glove::e37`。
- 复用项目已有的 standalone Asio 和 spdlog，不自行创建 I/O 线程。

## 目录结构

```text
E37/
├── CMakeLists.txt
├── README.md
├── README.zh-CN.md
├── include/
│   └── HSMS/
│       ├── Types.h
│       ├── Endpoint.h
│       ├── Connection.h
│       ├── MessageCodec.h
│       ├── ProtocolStateMachine.h
│       └── TransactionManager.h
└── src/
    ├── Types.cpp
    ├── Endpoint.cpp
    ├── Connection.cpp
    ├── MessageCodec.cpp
    ├── ProtocolStateMachine.cpp
    └── TransactionManager.cpp
```

公开声明位于 [include/HSMS/](./include/HSMS/)，函数实现位于 [src/](./src/)。
`Result<T>` 类型别名和聚合类型的成员默认值保留在头文件中。

## 实现范围与协议模式

实现依据为仓库中的 [SEMI E37-0222](../../docs/semi/E037-00-0222.PDF)。
默认的 HSMS-SS 模式依据 [SEMI E37.1-0702](../../docs/semi/E037-01-0702.PDF)；
该版本标有 **SUPERSEDED（已被后续版本替代）**。

本库不声明符合更新版本的 E37.1、HSMS-GS，也不代表通过 SEMI 认证。

| 行为 | `Mode::Generic` | `Mode::HsmsSS`，默认模式 |
| --- | --- | --- |
| 发起 Select | 双方均可，应用显式调用 | Active 端自动发起 |
| Select 的 Session ID | 配置的 16 位 Session ID | 固定为 `0xffff` |
| Deselect | 支持，包括双方同时发起 | 禁止使用 |
| Linktest | TCP 已连接时可用 | 仅在 Selected 状态可用 |
| Separate | 结束指定会话，保留 TCP 连接 | 结束选择并关闭 TCP |
| Select 失败 | 返回失败状态，不因此直接断开 TCP | 关闭 TCP |
| 数据消息路由 | 每个 ID 分别完成 Select | 所有配置的 15 位 Device ID 共享选择状态 |
| Reject | 支持 | 支持，未提供关闭此能力的选项 |

需要 Generic 模式时，应显式设置 `config.mode = Mode::Generic`。
Generic 模式采用“每个 Session ID 分别进行 Select”的关联约定。
E37 将具体会话关联方式留给下级标准，因此对端必须同意这一约定。
一条 TCP 连接可以独立选择多个 Generic 会话。

HSMS-SS 的选择状态通知使用 `0xffff`，但数据消息必须使用 Device ID。
本实现中，HSMS-SS 控制消息的 Session ID 均为 `0xffff`，包括 Reject。

当前只实现 PType 0，标准控制消息由库自动处理：

- 不支持的 PType/SType，以及没有对应开放事务的控制响应，会触发 Reject。
- 非法消息长度、消息头以及非法的 HSMS-SS 操作会导致连接关闭。
- 对于数据消息中未配置的 ID，当前实现会报告“不支持的会话”并关闭连接。这是本实现的处理策略，应配置好需要服务的全部 Device ID。

### 不包含的上层功能

数据消息体是拥有独立存储的原始字节数组，本库不解释其 SECS-II 内容。
以下功能属于应用层或 E5 层：

- SECS-II Item 编解码。
- Stream/Function 对应的业务语义。
- GEM 业务流程。
- Host/Equipment 角色。
- 按适用规范生成 S9Fx 错误消息。

特别是，设备端应用在 E37.1 要求的 T3 超时场景下，需要由上层生成 S9F9。
T3 错误的 `ErrorInfo::header` 提供原始请求头，
`MessageCodec::encode_header()` 可以生成用于 MHEAD/SHEAD 的十字节头部。
如果要声明设备级协议符合性，不能省略这些上层处理。

## 核心类职责

| 类 | 职责 |
| --- | --- |
| `Endpoint` | 主动连接或被动监听、连接数量控制、T5 与重连、连接截止时间、统一停止 |
| `Connection` | 单条 TCP 连接、有限发送队列、公开通信接口、T7/T8、周期 Linktest、回调与协议流程协调 |
| `MessageCodec` | 增量分帧、网络字节序编解码、消息格式校验 |
| `ProtocolStateMachine` | 连接状态、会话选择、操作合法性、同时 Select/Deselect 的规则 |
| `TransactionManager` | System Bytes 分配、响应匹配、T3/T6、取消、事务恰好完成一次 |

应用通常只需要直接使用 `Endpoint` 和 `Connection`。
另外三个类属于内部实现组件，其声明保留在头文件中以便独立测试，不保证独立使用时的线程安全。

## 生命周期与线程约定

1. 准备执行器、`Config` 和回调，调用 `Endpoint::create()`。
2. 检查创建结果，再调用 `async_start()` 并运行执行器。
3. 启动完成回调中的 `StartInfo` 只表示本地启动结果：
   - Passive 模式提供实际监听地址，端口配置为 0 时由系统分配临时端口。
   - Active 模式没有监听地址；启动成功**不代表 TCP 已连接**。
4. `on_connection` 在协议启动前交付新的 `shared_ptr<Connection>`。
   等待 `on_state_changed` 通知 `Selected` 后再发送业务数据。
   Generic 模式还需要显式调用 `async_select()` 选择所需会话。
5. 退出时调用 `Endpoint::async_stop()`，继续运行执行器，直到停止完成后再销毁 I/O 上下文。

### 线程与回调

- 公开异步接口可以从任意线程调用。
- 一个 Endpoint 及其全部 Connection 共享同一个 Asio strand，同一端点内的回调不会并发执行。
- 不同 Endpoint 的回调可以并发执行。
- 公开异步操作通过投递任务执行，不在调用栈内直接触发完成回调。
- `state()` 返回原子状态快照；会话状态通过 `async_session_selected()` 查询。
- 一条 Connection 的本地和远端地址在对象生命周期内保持不变。
- 回调不能阻塞 I/O，也不能抛出异常；可以在回调中继续调用公开异步接口。
- 收到的消息拥有自己的消息体存储，可以移交其他线程处理。
- 需要保留连接时可使用 `shared_from_this()`；回调捕获连接或端点时应避免引用环，优先使用弱引用。

### 停止行为

`async_stop()` 支持重复调用。停止 Endpoint 时会停止监听和重连，
对已选择的会话安排 Separate，并用 T6 限制正常停止时的发送等待时间。
如果停止过程失败，会显式返回错误，不会伪装成成功。

已有操作会在停止完成回调之前结束。停止后不再产生新的主动业务事件；
应用在停止后新提交的发送等操作仍会收到相应的失败结果。

**仅停止 Connection 不会关闭所属 Endpoint 的自动重连策略。**
要彻底停止通信，应停止 Endpoint。

## 接口说明

### 创建、启动和查询

| 接口 | 说明 |
| --- | --- |
| `Endpoint::create(executor, config, callbacks)` | 校验配置并创建端点，返回 `Result<std::shared_ptr<Endpoint>>` |
| `Endpoint::async_start(completion)` | 启动监听或安排主动连接，返回 `Result<StartInfo>` |
| `Endpoint::async_stop(completion)` | 停止端点及其连接 |
| `Connection::state()` | 获取连接状态快照 |
| `Connection::async_session_selected(id, completion)` | 异步查询指定会话是否已选择 |
| `Connection::local_endpoint()` / `remote_endpoint()` | 获取该连接的地址 |
| `Connection::async_stop(completion)` | 停止这一条连接 |

### 控制服务和数据服务

| 接口 | 结果或行为 |
| --- | --- |
| `async_select(id, completion)` | 成功，或包含远端状态码的 `SelectFailed` |
| `async_deselect(id, completion)` | 成功，或包含远端状态码的 `DeselectFailed` |
| `async_linktest(completion)` | 等待匹配响应，或返回拒绝、超时等错误 |
| `async_separate(id, completion)` | 返回本地发送结果；该服务没有远端确认消息 |
| `async_send(primary, completion)` | 发送奇数 Function 的 Primary，强制 W=0 |
| `async_request(primary, completion)` | 强制 W=1，立即返回本地 `TransactionId`，通过回调返回回复或错误 |
| `async_reply(incoming, function, body, completion)` | 回复收到的请求，自动继承原 Session ID、Stream 和 System Bytes |
| `cancel(id, completion)` | 取消本地等待；若请求尚未开始写入，会将其从发送队列移除 |

System Bytes 由库分配，调用方无需填写。
分配时会避开所有已保留或开放事务的标识，以及最近完成的标识。
数据回复按 Session ID、Stream、Function 和 System Bytes 匹配。

Function 0 回复会作为数据回复返回，此时 `header.byte3 == 0`。
应用应将其视为中止回复，而不是正常业务成功。

`IncomingPrimary` 携带的回复凭据：

- 绑定到接收请求的 Connection，只能消费一次。
- 不能用于回复 W=0 的请求，也不能跨重连使用。
- 本地校验或入队失败时不会被消费。

取消操作不能撤回已经发送的字节。
迟到或不匹配的回复不会结束其他事务，也不会重置其 T3。

**发送成功只表示字节已被本地 TCP 接受，不代表对端已经处理。**
`async_request()` 返回的 `TransactionId` 是取消句柄，不是线上的 System Bytes，
也不代表事务已经成功登记或发送。

## 回调与错误

`EndpointCallbacks` 提供 `on_connection`、端点级 `on_error`，
以及用于连接的 `Callbacks` 集合：

| 回调 | 用途 |
| --- | --- |
| `on_state_changed` | 连接状态变化及原因 |
| `on_session_state_changed` | 会话选择状态变化 |
| `on_primary_received` | 处理收到的业务请求，W=1 时按业务需要调用 `async_reply()` |
| `on_select_requested` | 同步、快速决定是否接受 Select；返回原始状态码，0 表示接受 |
| `on_deselect_requested` | 同步、快速决定是否接受 Deselect，例如业务忙碌时拒绝 |
| `on_reject_received` | 观察远端 Reject |
| `on_unmatched_reply` | 观察迟到或无法匹配的数据回复 |
| `on_trace` | 可选的方向、消息头及消息体长度跟踪，不复制或记录消息体 |
| `on_error` | 报告连接、协议等错误 |

默认接受合法的 Select/Deselect。
双方同时发起操作时，协议强制规则优先于应用策略回调。

操作结果使用 `std::expected<T, ErrorInfo>`，每个已提供的操作完成回调恰好调用一次。
Select/Deselect 的非零状态是错误，不作为成功结果返回。

同一个致命连接错误可能通过连接错误事件通知一次，同时使多个受影响的操作失败。
缺少相应错误或诊断回调时，库会使用 spdlog 记录错误；
未注册 Primary 处理回调时，也会显式报告缺少处理器。

## 参数、限制与持久化

| 配置 | 默认值 | 允许范围或说明 |
| --- | --- | --- |
| `mode` | `Mode::HsmsSS` | Generic 或 HSMS-SS |
| `connect_mode` | `ConnectMode::Active` | Active 或 Passive，与 Host/Equipment 角色无关 |
| `address` / `port` | `127.0.0.1` / `5000` | Passive 为监听地址，Active 为目标地址 |
| `timeouts.t3` | 45 秒 | 1～120 秒，1 秒分辨率 |
| `timeouts.t5` | 10 秒 | 1～240 秒，1 秒分辨率 |
| `timeouts.t6` | 5 秒 | 1～240 秒，1 秒分辨率 |
| `timeouts.t7` | 10 秒 | 1～240 秒，1 秒分辨率 |
| `timeouts.t8` | 5 秒 | 1～120 秒，1 秒分辨率 |
| `max_receive_length` / `max_send_length` | 各 16 MiB | 至少 10，受 32 位消息长度字段限制 |
| `max_queued_bytes` | 32 MiB | 至少 14，包含四字节长度前缀 |
| `max_transactions` | 1024 | 至少 1，数据和控制事务合计 |
| `max_connections` | 1 | 至少 1，被接纳的连接各自维护协议状态 |
| `connect_timeout` | 10 秒 | 正整数秒，包含 DNS 解析时间 |
| `linktest_interval` | 0，关闭 | 0 表示关闭，否则为正整数秒 |
| `reconnect` | `true` | 仅用于 Active 模式 |
| `session_ids` | `{0}` | 非空、不重复；Generic 为 16 位 ID，SS 为 0～32767 |

消息长度包含十字节消息头，**不包含**四字节长度前缀。
默认最大消息体为 16 MiB 减十字节。
发送还受 `max_queued_bytes` 限制；库不会把一条消息自动拆成多条 HSMS 消息。
`max_send_length` 也是本实现预期发送消息大小的上限。

### 定时器语义

- **T3**：完整请求写入后开始等待数据回复，每个事务独立计时；超时只结束该事务。
- **T5**：限制主动连接尝试的间隔。尝试成功或失败后至少间隔 T5；断线也会启动 T5 延迟。
- **T6**：完整控制请求写入后开始计时；控制事务超时会关闭连接。发送排队时间不计入 T3/T6。
- **T7**：限制 NotSelected 状态停留时间。根据仓库中的 SS 状态表，SS Active 端等待 Select 的 T6。
- **T8**：限制一条未收完整消息内部的接收间隔，包括不完整的长度前缀；有接收进展时刷新。它不是连接空闲定时器。
- 周期 Linktest 是独立、可选的功能，不等同于上述协议定时器。

### 连接容量与配置保存

Passive 端达到连接数量上限后暂停接受新连接，有空位后继续接受。
这对应 E37 §9.2.4.1.1 中不继续接纳连接的选项。
处于 TCP backlog 的连接仍可能完成 TCP 握手，但在被应用接纳前不会获得 HSMS 服务，对端可能超时。

端点运行期间配置不变。上层应用负责将配置持久化，在断电或软件重启后恢复，
并在调用 `create()` 前加载配置、处理校验失败。通信库不绑定具体文件格式。

Passive 地址必须是 IPv4/IPv6 数字地址；Active 地址也可以使用 DNS 名称。
Active 模式不允许端口 0。

## 最小 Active HSMS-SS 示例

下面的示例连接到目标设备，选择完成后发送只有消息头的 S1F1，收到回复或错误后停止。
运行前需要有可连接的 HSMS-SS 对端；回复消息体的 SECS-II 解析由应用另行完成。

```cpp
#include <HSMS/Endpoint.h>
#include <asio/io_context.hpp>
#include <iostream>

int main() {
    using namespace glove::e37;
    asio::io_context io;
    Config config;
    config.mode = Mode::HsmsSS;
    config.address = "127.0.0.1";
    config.port = 5000;
    config.reconnect = false;

    std::shared_ptr<Endpoint> endpoint;
    const auto stop = [&] {
        endpoint->async_stop([](Result<void> result) {
            if (!result) std::cerr << result.error().message << '\n';
        });
    };
    EndpointCallbacks callbacks;
    callbacks.on_error = [&](const ErrorInfo& failure) {
        std::cerr << failure.message << '\n';
        stop();
    };
    callbacks.connection.on_error = [&](Connection&, const ErrorInfo& failure) {
        std::cerr << failure.message << '\n';
        stop();
    };
    callbacks.connection.on_state_changed = [&](Connection& connection, const StateChange& change) {
        if (change.current != State::Selected) return;
        DataMessage primary{{0, 1, 1, 0, 0, 0}, {}};
        connection.async_request(std::move(primary), [&](Result<DataMessage> reply) {
            if (!reply) std::cerr << reply.error().message << '\n';
            else std::cout << "Reply function: " << unsigned(reply->header.byte3) << '\n';
            stop();
        });
    };
    auto created = Endpoint::create(io.get_executor(), config, std::move(callbacks));
    if (!created) {
        std::cerr << created.error().message << '\n';
        return 1;
    }
    endpoint = *created;
    endpoint->async_start([&](Result<StartInfo> result) {
        if (!result) {
            std::cerr << result.error().message << '\n';
            stop();
        }
    });
    io.run();
}
```

作为同一 CMake 工程中的使用方，可链接：

```cmake
target_link_libraries(your_target PRIVATE Glove::E37)
```

Passive 端设置 `connect_mode = ConnectMode::Passive`，
注册 `on_primary_received`，由应用生成 SECS-II 回复消息体。
示例只展示通信流程；打印回复 Function 不等于确认业务成功，尤其应单独处理 Function 0。

## 构建与验证

在仓库根目录执行。已有构建目录时可跳过配置步骤：

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build --target Tests
ctest --test-dir build -R E37 --output-on-failure
```

测试覆盖消息分帧和精确字节、两种模式、回复匹配、同时发起控制操作、多 ID、
队列与事务限制、取消、T3/T5/T6/T7/T8、本地 TCP 收发、Reject、重连、停止及多线程调用。

### AddressSanitizer / UndefinedBehaviorSanitizer

在支持这些选项的编译器和平台上，可以使用以下配置。
同时编译仓库内的 GoogleTest 和 spdlog，避免其 C++ 容器代码与本库使用不一致的检测配置：

```sh
cmake -S . -B build/e37-sanitize -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined" \
  -DCMAKE_DISABLE_FIND_PACKAGE_GTest=TRUE \
  -DCMAKE_DISABLE_FIND_PACKAGE_spdlog=TRUE
cmake --build build/e37-sanitize --target Tests
ctest --test-dir build/e37-sanitize -R E37 --output-on-failure
```
