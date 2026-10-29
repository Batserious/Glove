# E37 / HSMS

[English](./README.md) | [简体中文](./README.zh-CN.md)

C++23 asynchronous HSMS transport and transaction library. Link `Glove::E37` and
include `<HSMS/Endpoint.h>`. The library reuses standalone Asio and spdlog from
the parent build; it does not own an I/O thread.

Public declarations are in `include/HSMS/*.h`; all function implementations are
in `src/*.cpp`. The `Result<T>` type alias and aggregate data-member defaults
remain in the headers.

## Scope and profiles

The implementation is based on the repository's SEMI E37-0222 document.
`Mode::HsmsSS` is the default; its restrictions follow the supplied E37.1-0702 document,
which is marked **SUPERSEDED**. This is not a claim of conformance to a newer
E37.1 edition, HSMS-GS, or certification by SEMI.

| Behavior | `Mode::Generic` | `Mode::HsmsSS` (default) |
| --- | --- | --- |
| Select initiation | Either side, explicitly requested | Active side automatically |
| Select session ID | Configured 16-bit session ID | `0xffff` |
| Deselect | Supported, including simultaneous requests | Forbidden |
| Linktest | Any connected state | Selected only |
| Separate | Ends the specified session; TCP remains open | Ends selection and closes TCP |
| Select failure | Returns status; does not itself close TCP | Closes TCP |
| Data routing | Each ID must have its own successful Select | All configured 15-bit Device IDs share selection |
| Reject | Supported | Supported, not disabled |

Set `config.mode = Mode::Generic` explicitly to use Generic mode.
Generic mode uses an explicit one-Select-per-session-ID association. E37 leaves
session association to subordinate standards, so peers must agree to this
convention. Multiple generic sessions are independently selectable on one
connection. SS selection notifications use `0xffff`; data messages use Device
IDs, not `0xffff`. SS control headers, including Reject, use `0xffff`.

Only PType 0 is implemented. Standard control messages are handled internally.
Unsupported PType/SType and unmatched control responses generate Reject.
Malformed lengths/headers and invalid SS procedures close the connection.
Incoming data with an unconfigured ID is reported as an unsupported session
and closes the connection; configure every Device ID this endpoint services.

The body of a data message is opaque owned bytes. SECS-II Item encoding,
stream/function semantics, GEM, and Host/Equipment roles belong to the
application/E5 layer. In particular, an equipment application must implement
S9F9 on T3 expiry where required by E37.1. `ErrorInfo::header` contains the
original request, and `MessageCodec::encode_header()` produces the ten bytes
needed for MHEAD/SHEAD. Do not silently omit that upper-layer integration if
claiming equipment-level conformance.

## Classes

- `Endpoint`: listener or connector, connection admission, T5/reconnect,
  connect deadline, and aggregate shutdown.
- `Connection`: socket, bounded write queue, public services, T7/T8, heartbeat,
  callbacks, and protocol orchestration.
- `MessageCodec`: incremental framing and network-byte-order encoding.
- `ProtocolStateMachine`: state, session selection, legal procedures, and
  simultaneous Select/Deselect rules.
- `TransactionManager`: System Bytes, reply association, T3/T6, cancellation,
  and exactly-once completion.

The last three classes are implementation components, exposed in headers for
focused testing. They are not independently thread-safe. Applications should
normally use `Endpoint` and `Connection`.

## Lifecycle and threading

1. Create an `Endpoint` with an executor, validated `Config`, and callbacks.
2. Call `async_start()` and run the executor.
3. `StartInfo` acknowledges local startup. Passive mode provides the bound
   endpoint (port 0 requests an ephemeral port); Active mode has no listening
   endpoint and startup does **not** imply a successful TCP connection.
4. `on_connection` provides a new `shared_ptr<Connection>` before protocol
   startup. Wait for `on_state_changed` to observe `Selected` before data I/O.
   Generic mode requires an explicit `async_select()` on each desired ID.
5. Call `Endpoint::async_stop()` and keep running the executor until completion
   before destroying the I/O context.

Public async calls may originate on any thread. Each endpoint and its
connections share one Asio strand; callbacks never run concurrently within
that endpoint. Separate endpoints may execute concurrently. Public operations
post work and do not invoke completions inline. State queries use `state()`
(atomic snapshot) or `async_session_selected()`. Local/remote addresses are
immutable for the lifetime of a connection.

Callbacks must not block or throw. They may post more asynchronous operations.
Incoming messages own their body storage and may be transferred to another
thread. Retain a connection using `shared_from_this()` when necessary.
Avoid ownership cycles in callbacks; prefer weak captures.

`async_stop()` is idempotent. It disables reconnect/listening, queues Separate
for selected sessions, and bounds graceful draining by T6. A drain failure is
reported, not converted into successful shutdown. Outstanding operations are
completed before stop completion; no further unsolicited business events are
generated. Operations explicitly submitted after stopping still complete with
an error. Stopping a **connection** alone does not disable its endpoint's
reconnection policy; stop the endpoint to shut down permanently.

## Data and control API

| API | Result |
| --- | --- |
| `async_select(id, completion)` | Success or `SelectFailed`, including remote status |
| `async_deselect(id, completion)` | Success or `DeselectFailed`, including remote status |
| `async_linktest(completion)` | Matching response, rejection, or failure |
| `async_separate(id, completion)` | Local send completion; there is no peer acknowledgment |
| `async_send(primary, completion)` | Sends an odd-function Primary, forces W=0 |
| `async_request(primary, completion)` | Forces W=1; returns local `TransactionId` immediately |
| `async_reply(incoming, function, body, completion)` | Inherits the original session, stream and System Bytes |
| `cancel(id, completion)` | Cancels local waiting; removes a queued request if writing has not begun |

The library assigns System Bytes; callers need not populate them. It avoids
all reserved/open transactions and the most recently completed identifier.
Replies match session, stream, function, and System Bytes. Function zero is
returned as a data reply with `header.byte3 == 0`; the application must treat it
as an abort reply rather than normal business success.

Reply tickets are tied to one connection and can be consumed only once. They
cannot reply to W=0 requests or be reused after reconnection. A failed local
validation/enqueue does not consume a ticket. Cancellation never retracts
already transmitted bytes. Late/mismatched data replies do not complete a
different transaction or reset its T3.

Successful sends mean bytes were accepted by local TCP, not that the peer
processed them. A returned `TransactionId` is a cancellation handle, not the
wire System Bytes, and does not imply that registration or sending succeeded.

## Callbacks and errors

`EndpointCallbacks` provides `on_connection`, endpoint `on_error`, and the
per-connection `Callbacks` bundle:

- `on_state_changed`, `on_session_state_changed`: lifecycle observations.
- `on_primary_received`: business requests; use `async_reply()` for W=1.
- `on_select_requested`, `on_deselect_requested`: quick synchronous admission
  decisions returning raw status (zero accepts). Defaults accept; mandatory
  simultaneous-operation rules take precedence over these decisions.
- `on_reject_received`, `on_unmatched_reply`: protocol diagnostics.
- `on_trace`: optional direction/header/body-size observations, without copying
  or logging message bodies.
- `on_error`: connection/protocol faults not owned solely by an operation.

Each supplied operation completion is invoked exactly once. Results use
`std::expected<T, ErrorInfo>`. Nonzero Select/Deselect statuses are errors, not
success-shaped responses. A fatal connection error may appear once as a
connection event and also complete multiple affected operations with failure.
Missing error/diagnostic handlers fall back to spdlog, not silent failure.
Missing primary handlers produce an explicit unsupported-handler diagnostic.

## Parameters, limits, and persistence

| Setting | Default | Accepted values |
| --- | --- | --- |
| Protocol mode | `Mode::HsmsSS` | `Mode::HsmsSS` or `Mode::Generic` |
| T3 | 45 s | 1..120 s, 1 s resolution |
| T5 | 10 s | 1..240 s, 1 s resolution |
| T6 | 5 s | 1..240 s, 1 s resolution |
| T7 | 10 s | 1..240 s, 1 s resolution |
| T8 | 5 s | 1..120 s, 1 s resolution |
| Receive/send message length | 16 MiB each | At least 10; 32-bit wire length |
| Queued send bytes | 32 MiB | At least 14, includes length prefixes |
| Open transactions | 1024 | At least 1, data and control combined |
| Accepted connections | 1 | At least 1, independent protocol state |
| Connect deadline | 10 s | Positive whole seconds, includes DNS |
| Periodic Linktest | Disabled | Zero disables; otherwise positive whole seconds |
| Reconnect | Enabled | Active mode only |
| Session/Device IDs | `{0}` | Nonempty, unique; SS: 0..32767 |

Message lengths include the ten-byte header but not the four-byte length
prefix. Thus the default maximum body is 16 MiB minus ten bytes. Outgoing
messages are bounded by both `max_send_length` and `max_queued_bytes`; there is
no implicit splitting into multiple HSMS messages. The configured send limit
is also this implementation's maximum expected outgoing message size.

T3/T6 start after the complete request is written, not when queued. Each
transaction has its own timer. T3 closes only that transaction. T6 closes the
connection. T7 limits NotSelected residence (SS Active waits on Select's T6,
as specified in the supplied SS state table). T8 is refreshed by progress
inside an incomplete frame, including a partial length prefix. It is not a
connection-idle timer. Periodic Linktest is a separate optional facility.

At capacity, a Passive endpoint pauses accepting further connections and
resumes when a slot is released (the non-accepting option in E37 section
9.2.4.1.1). TCP backlog connections may still complete their TCP handshake but
will not receive HSMS service until admitted; peers may time out. Active
attempts are separated by at least T5 after completion/failure; a disconnection
also starts a T5 delay.

Configuration is immutable while an endpoint runs. The embedding application
must persist it across power loss/reload, load it before `create()`, and handle
validation errors. The library intentionally does not choose a storage format.
Passive addresses are numeric IPv4/IPv6 literals; Active addresses may also
be DNS names. Active port zero is invalid.

## Minimal active HSMS-SS example

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
        DataMessage primary{{0, 1, 1, 0, 0, 0}, {}}; // Header-only S1F1.
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

For a Passive endpoint, set `connect_mode = ConnectMode::Passive` and register
`on_primary_received`. Generate SECS-II reply bodies in the application layer.

## Validation

```sh
cmake --build build --target Tests
ctest --test-dir build -R 'E37' --output-on-failure
```

Tests cover framing and exact bytes, both profiles, reply matching, simultaneous
control procedures, multiple IDs, queue/transaction limits, cancellation,
T3/T5/T6/T7/T8, local TCP round trips, remote rejection, reconnection, shutdown,
and calls from multiple application threads.

An instrumented build can also use the bundled GoogleTest and spdlog so that
their C++ container code is instrumented consistently with the library:

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
