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

[IMPLEMENTED — v5.5 S1] [enum 선언 순서](../kcf/include/kcf/system/application_state.hpp)는
`INITIALIZING, RUNNING, ERROR, SHUTTING_DOWN, RESETTING, STOPPED`입니다.
STOPPED는 기존 숫자값을 유지하도록 마지막에 추가했습니다. 선언 순서는 상태 전이 순서가 아닙니다.

| 상태 | 계약 |
| --- | --- |
| INITIALIZING | Supervisor 준비 및 Managed 자식 실행, Runtime RUNNING barrier 대기 |
| RUNNING | 모든 자식의 초기 RUNNING 확인 후 process exit, Runtime 오류, 응답 timeout, heartbeat stall 감시 |
| ERROR | 초기 자식 실패 또는 운전 중 장애를 기록하고 사용자 명령 대기. 나머지 정상 Managed 프로세스 유지·감시 |
| RESETTING | 명시적 RequestReset으로 전체 Managed generation 종료·회수 후 같은 Spec/Profile로 재실행 |
| SHUTTING_DOWN | 명시적 Shutdown 또는 SIGINT/SIGTERM에 따라 관리 자식 종료·회수 중 |
| STOPPED | 관리 자식 회수 완료 후 게시하는 종료 상태. 종료 요청만으로 미리 진입하지 않음 |

### Setup 반환값과 초기 실패

`Bringup::Setup()`의 0은 **Supervisor 준비 완료 및 Run 진입 가능**을 의미하며 RUNNING 보장이 아닙니다.
초기 Element Setup 실패, 자식 실행 실패 또는 startup barrier 실패는 Application ERROR로 관리합니다.
이미 실행 중인 정상 자식은 유지하며, `Run()`에서 감시 및 사용자 Reset/Shutdown 요청을 처리합니다.
프로세스 시작 자체가 실패하면 해당 generation에서 뒤에 있는 Element는 아직 시작되지 않을 수 있습니다.

등록 형식 검증 및 Supervisor 자체 준비 실패는 오류를 반환하고 준비 과정에서 획득한 자원을 정리합니다.
초기화 중 명시적 종료 요청은 자식 정리 후 `-ECANCELED`를 반환합니다.
등록 검증은 [ValidateElements 사용법](05_APPLICATION_DEVELOPMENT_GUIDE.md#element-registration-validation)을 참조합니다.

### Current Error와 내부 History

하나의 Managed Element 장애도 Application ERROR의 원인이 됩니다.
`GetApplicationError()`는 최초 Current Error를 보존하고, 이후 다른 Element의 장애는
`GetErrorHistory()`에 누적합니다. 같은 Element/generation 장애의 반복 폴링은 중복 기록하지 않습니다.
History는 발생 순서, Monotonic timestamp, generation, Element 이름/PID, failure_kind,
runtime_error 관측 여부 및 exit 정보를 담으며 **최근 64건**을 유지합니다. 나중에 회수한 exit 정보는
기존 항목에 보완할 수 있습니다. 최초 Current Error는 History의 오래된 항목이 제거돼도 유지됩니다.

Runtime 오류를 실제 응답에서 관측했는지는 `runtime_error_observed`로 구분합니다.
Runtime 응답 전에 자식이 종료되면 exit 정보만 남을 수 있으며, `-ECHILD`를 실제 Setup 오류 코드로 추정하지 않습니다.
History는 Supervisor 내부 조회 전용이며 외부 IPC/Network로 공개하지 않습니다.
상태·오류·History 조회는 lifecycle을 실행하는 동일 제어 thread에서 사용합니다.

### 명시적 Reset과 Shutdown

- `RequestReset()`은 ERROR에서만 사용자 명령으로 사용합니다. 자동 Reset/Restart는 없습니다.
- Reset은 전체 Managed 프로세스를 종료·회수한 뒤 등록된 Spec/Profile로 새 generation을 시작합니다.
  성공하면 RUNNING으로 전환하고 Current Error만 해제하며 History는 유지합니다.
- Reset 재초기화 실패 시 해당 시도의 자식을 정리하고 ERROR로 돌아갑니다. 최초 Current Error와
  새 실패 History를 보존하며 다음 사용자 요청 없이 재시도하지 않습니다.
- `RequestShutdown()`은 INITIALIZING/RUNNING/ERROR/RESETTING에서 종료를 요청하며 SIGINT/SIGTERM과
  같은 정상 종료 경로를 사용합니다. Reset 중에도 Shutdown이 우선하고 중복 종료 요청은 안전하게 처리합니다.
- 정상 Shutdown은 오류가 아닌 기능입니다. 종료 요청 및 그에 따른 자식 종료를 새 장애로 기록하지 않습니다.
  기존 Current Error/History는 보존하므로, 정상 정리가 완료돼도 이전 장애 때문에 Run 반환값은 nonzero일 수 있습니다.
- 자식 회수 완료 후 SHUTTING_DOWN → STOPPED를 게시하고 SystemStatus SHM을 정리합니다.
  최신값 Snapshot에서는 중간 상태를 건너뛸 수 있으며, SHM 제거 이후 STOPPED의 영구 관측은 보장하지 않습니다.

[ElementSpec](../bringup/include/bringup/bringup.hpp) 기본 timeout: startup 5000 ms,
health 2000 ms, shutdown 1500 ms. 종료 기한을 넘긴 child는 controlled ForceStop/reap 대상입니다.
[FailureKind](../kcf/include/kcf/system/element_failure_kind.hpp)는 NONE, PROCESS_EXIT,
RUNTIME_ERROR, STATUS_TIMEOUT, HEARTBEAT_STALL입니다.

RequestReset/RequestShutdown은 thread-safe 요청이며 signal-handler API가 아닙니다.
이를 호출할 사용자 입력 경로는 Application이 연결해야 합니다. API 존재만으로 예제 Bringup main이나 Tool에
일반 사용자용 Reset/Shutdown 버튼·명령이 구현됐다고 해석하지 않습니다.

### Managed / External / Attached

Managed는 ElementSpec으로 등록해 Supervisor가 실행·감시·종료·회수·Reset하는 프로세스입니다.
Standalone/External은 독립 실행되며 Supervisor 등록 없이 Framework IPC를 사용할 수 있습니다.
Supervisor는 External PID를 관리하거나 종료하지 않고, External 장애를 Application ERROR로 자동 전파하지 않습니다.
External 데이터를 필수 입력으로 사용하는 경우 유효성·Timeout·안전 동작 판단은 해당 Application의 책임입니다.

Supervisor 종료·Reset은 External의 직접 종료를 의미하지 않지만 IPC 공급 중단·SHM 세대 변경은
외부 소비자에 영향을 줄 수 있습니다. 프로세스 소유권과 데이터 의존성은 별개입니다.
[Composition의 명시적 재연결](05_APPLICATION_DEVELOPMENT_GUIDE.md#external-observer의-명시적-shm-재연결)은
External PID를 유지하면서 사용자가 새 SHM에 연결하는 예입니다.
Attached API는 현재 미구현이며 구체적인 요구 전까지 구현을 보류합니다.
기존 Introspection/SystemStatus 관측이 외부 PID의 실행·종료 소유권을 부여하지는 않습니다.

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
