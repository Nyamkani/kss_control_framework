# 02. Runtime and Supervision

## Process / OS lifecycle

[IMPLEMENTED] [process.cpp](../kcf/src/process.cpp)는 argv/env를 fork 전에 준비합니다.
`socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK)`로 supervision 경로를 만들고
child FD를 exec에 전달합니다. `execve` 실패는 CLOEXEC pipe로 부모에게 알립니다.
Process::Start 성공은 exec 성공이며, Setup 완료 또는 RUNNING 보장이 아닙니다.

RequestStop은 정상 SIGTERM 요청, ForceStop은 명시적 SIGKILL, Wait/IsRunning의 collect는 child reap을 담당합니다.
Process destructor는 FD만 닫으며 child를 자동 kill/reap하지 않습니다. PID는 단일 owner가 관리합니다.

## Runtime supervision

[RuntimeStatus protocol](../kcf/include/kcf/process/runtime_supervision.hpp): version 1,
request 16 bytes / response 40 bytes. response에는 request_id, PID, ProcessState, loop_heartbeat,
runtime_error가 있습니다. 이것은 introspection registry snapshot과 별개입니다.

Worker는 Setup/Loop와 독립적으로 요청에 응답합니다. 성공 Loop만 heartbeat를 증가시키므로
응답이 살아 있어도 heartbeat stall은 구분할 수 있습니다. 유효 요청이 2000 ms 동안 없으면
-ETIMEDOUT, disconnect/error는 -ECONNRESET loss를 latch합니다. Worker가 Element Shutdown을 호출하지는 않습니다.

Failure precedence: stop_reason은 0(없음), 1(정상 stop), 음수(loss)입니다.
이미 signal/정상 stop이 선점했으면 이후 loss가 덮어쓰지 않습니다. 반대로 음수 loss가 먼저 latch되면
Setup/Loop 결과보다 우선합니다. Shutdown exception은 기존 오류를 보존합니다.
[Run 코드](../kcf/src/process_runtime.cpp)를 기준으로 하며 단순히 “마지막 오류가 우선”인 계약이 아닙니다.

[CURRENT LIMITATION] Setup/Loop가 반환하지 않으면 worker가 loss를 감지해도 Run의 cleanup이 진행되지 않습니다.
Runtime discovery는 전이 시 갱신되므로 이전 RUNNING이 남을 수 있습니다. RUNNING metadata는 실시간 progress 증명이 아닙니다.
Unix exit status는 negative errno 전체를 보존하지 않습니다. 정확한 error는 직접 Run 반환값/status로 검증합니다.

## Supervisor states and Reset

정확한 [enum](../kcf/include/kcf/system/application_state.hpp):
`INITIALIZING, RUNNING, ERROR, SHUTTING_DOWN, RESETTING`.

- INITIALIZING: exec 후 각 Runtime RUNNING barrier를 기다림. startup 실패는 정리·실패 반환하며 반드시 RUNNING→ERROR를 거치지는 않음.
- RUNNING: process exit, Runtime 오류, 응답 timeout, heartbeat stall을 관찰.
- ERROR: 최초 ApplicationError를 latch. healthy child를 자동 종료/재시작하지 않음.
- RESETTING: ERROR에서 승인된 RequestReset 요청으로 기존 generation을 stop/reap하고 새 generation을 실행.
- 새 generation 모두 RUNNING이면 오류를 해제하고 RUNNING. 재초기화 실패는 ERROR.
- SIGINT/SIGTERM 종료는 SHUTTING_DOWN과 child 정리.

[ElementSpec](../bringup/include/bringup/bringup.hpp) 기본 timeout: startup 5000 ms,
health 2000 ms, shutdown 1500 ms. 종료 기한을 넘긴 child는 controlled ForceStop/reap 대상입니다.
[FailureKind](../kcf/include/kcf/system/element_failure_kind.hpp)는 NONE, PROCESS_EXIT,
RUNTIME_ERROR, STATUS_TIMEOUT, HEARTBEAT_STALL입니다.

RequestReset은 thread-safe 요청이며 signal-handler API가 아닙니다. 예제 Bringup main이나 Tool에
일반 사용자용 Reset 버튼/명령이 제공된다고 설명하지 않습니다. Reset 회귀 fixture는 별도 trigger를 사용합니다.

## Application identity / SystemStatus

[IMPLEMENTED] [SystemStatus](../kcf/include/kcf/system/system_status_channel.hpp)는 Supervisor instance
`PID + process_start_ticks` scope에 속합니다. child에는 scope가 전달됩니다.
동일 application_name이어도 다른 instance이며, 이름은 global identity가 아닙니다.
Reset은 Supervisor identity/scope를 유지하고 child generation을 교체합니다. Application 재실행은 Supervisor도 바뀝니다.
외부 observer는 `OpenForApplication(pid, start_ticks)`를 사용합니다. scope 없는 legacy Open은 global fallback이며
여러 Application 중 하나를 자동 선택하지 않습니다.

## 사례와 검증 경계

[AUTOMATED VERIFIED] Action app에서 startup barrier 완료 후 server SIGKILL → RUNNING→ERROR, cleanup,
새 Supervisor/child identity로 restart와 Success/Cancel 반복을 확인했습니다.
[AUTOMATED VERIFIED] scope 회귀에서 두 Supervisor, 같은 이름, state isolation, Reset을 확인했습니다.

[MANUALLY VERIFIED] 사용자 직접 PubSub 검증에서 subscriber SIGKILL → Supervisor RUNNING→ERROR,
healthy publisher 계속 실행, Tool Refresh 전 snapshot 유지 및 Refresh 후 failed subscriber 제거를 확인했습니다.
Process exit의 [RecordFailure](../bringup/src/bringup.cpp) 분류는 PROCESS_EXIT입니다.
Bringup 종료·재실행 후 새 Supervisor/child PID/start_ticks generation과 Standalone discovery도 확인했습니다.
이 결합 결과는 자동 fixture가 아닌 manual integration verification입니다.
Tool topology/state는 [수동 Refresh](04_INTROSPECTION_AND_TOOL.md)로 갱신됩니다.
