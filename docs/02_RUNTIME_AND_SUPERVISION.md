# 02. Runtime and Supervision

## Process / OS lifecycle

[IMPLEMENTED] [process.cpp](../kcf/src/process.cpp)는 argv/env를 fork 전에 준비합니다.
`socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK)`로 supervision 경로를 만들고
child FD를 exec에 전달합니다. `execve` 실패는 CLOEXEC pipe로 부모에게 알립니다.
Process::Start 성공은 exec 성공이며, Setup 완료 또는 RUNNING 보장이 아닙니다.

RequestStop은 정상 SIGTERM 요청, ForceStop은 명시적 SIGKILL, Wait/IsRunning의 collect는 child reap을 담당합니다.
Process destructor는 FD만 닫으며 child를 자동 kill/reap하지 않습니다. PID는 단일 owner가 관리합니다.

## ExecutionProfile — v5.4 로컬 개발 변경

[IMPLEMENTED] [ExecutionProfile](../kcf/include/kcf/process/execution_profile.hpp)은
`SchedulingPolicy::{INHERIT, OTHER, FIFO, RR}`, `priority`, `cpu_affinity`, `lock_memory`를 제공합니다.
기본값은 INHERIT / 0 / 빈 CPU 목록 / false로 기존 실행 동작을 유지합니다.
[Process::Start](../kcf/include/kcf/process/process.hpp)의 네 번째 선택 인자가 Profile입니다.

```cpp
kcf::ExecutionProfile profile;
profile.policy = kcf::SchedulingPolicy::OTHER;
profile.cpu_affinity = {0}; // 실제 장치에서 허용된 CPU를 선택
// profile.lock_memory = true; // 충분한 MEMLOCK 한도가 있는 경우에만 요청
process.Start(executable, arguments, system_status_scope, profile);
// Bringup에서는 ElementSpec의 마지막 멤버:
spec.execution_profile = profile;
```

- fork 전에 정책/priority/CPU ID를 검증합니다. INHERIT/OTHER priority는 0,
  FIFO/RR은 플랫폼의 허용 범위를 사용합니다. CPU ID는 cpu_set_t 및 configured CPU 범위 내여야 합니다.
- 빈 affinity는 상속합니다. 비어 있지 않으면 child의 execve 전에 sched_setaffinity를 적용하며,
  명시된 정책은 sched_setscheduler로 적용합니다. 실제 cpuset/온라인 CPU/권한 검사는 커널이 수행합니다.
- 실패하면 exec를 진행하지 않고 기존 실패 pipe 경로로 `-errno`를 반환하며 child를 회수합니다.
  FIFO/RR 권한 부족을 OTHER로 자동 fallback하지 않습니다. Start 성공은 여전히 exec 성공만 의미합니다.
- Bringup의 StartGeneration은 `ElementSpec::execution_profile`을 전달합니다.
  Reset도 같은 Profile로 새 generation을 실행하며 기존 aggregate 초기화를 유지합니다.

`lock_memory=true`는 exec 이전 mlock이 아닙니다. Process는 부모의 동명 환경변수를 제외하고
내부 `KCF_EXECUTION_LOCK_MEMORY=0/1`을 명시적으로 전달합니다. Runtime은 값을 검증·소비·제거하고,
exec 이후 Runtime/supervision 초기화 뒤 Element Setup 전에 `mlockall(MCL_CURRENT | MCL_FUTURE)`을 호출합니다.
잘못된 값은 `-EINVAL`, 잠금 실패는 `-errno`로 ERROR 처리하며 Setup/Loop/Shutdown을 호출하지 않습니다.
Setup에 진입한 이후의 종료에서는 기존 exactly-once Shutdown 계약을 유지합니다.
획득한 잠금은 Shutdown, worker join, Finalize/introspection 정리를 마친 뒤 해제합니다.

충분한 RT Priority/MEMLOCK 권한은 실행 환경의 책임입니다. API가 권한을 올리거나 한도를 조정하지 않습니다.
잠금 성공이 이후 모든 할당 성공이나 hard real-time을 보장하지 않으며,
외부 프로세스 관리·PREEMPT_RT 설치·CPU/IRQ 격리 정책은 제공하지 않습니다.

## Loop 주기와 지연 복구 — v5.4 로컬 개발 변경

정상 실행은 steady_clock 절대 시각 기준 `next_tick += period`와 `sleep_until(next_tick)`을 유지합니다.
성공 Loop 후 이미 다음 tick이 지났으면 `next_tick = now + period`로 재예약하고 그 시각까지 대기합니다.
밀린 주기를 보상하려고 즉시 Loop를 연속 실행하지 않습니다. 이전 `next_tick = now` 후 무대기 경로를 변경했습니다.
Loop 오류, 성공 heartbeat 증가, Stop/Supervisor loss 우선순위 및 Shutdown 경로는 변경하지 않았습니다.

지연 후 한 주기를 기다리므로 평균 실행 Hz는 낮아질 수 있습니다. 정상 절대 시각 처리 중에는
1주기보다 짧은 간격이 여전히 생길 수 있으며, 모든 Loop 간격의 최솟값을 보장하는 API는 아닙니다.
20ms 주기에서 70ms 초과 Loop 후 최소 20ms 대기를 확인하는 기존 lifecycle 테스트에 회귀를 추가했습니다.
1kHz VM 측정과 실장 미검증 범위는 [검증 기록](06_VERIFICATION_AND_LIMITATIONS.md#v54-local-integration)에 구분합니다.

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
