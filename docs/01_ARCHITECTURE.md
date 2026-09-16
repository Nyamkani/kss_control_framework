# 01. Architecture

[USER REQUIREMENT / DECISION] 실행/통신 기반은 Framework, 실제 제어·FSM·Driver 및 안전 판단은 Application에 둡니다.

| 계층/용어 | 현재 실체 | 책임 |
| --- | --- | --- |
| Application | 이름과 ElementSpec 집합 | 실제 시스템 구성 |
| Bringup / Supervisor | [bringup::Bringup](../bringup/include/bringup/bringup.hpp) | 관리 process 실행·관찰·Reset·정리; 별도 generic Supervisor class를 뜻하지 않음 |
| Process | [kcf::Process](../kcf/include/kcf/process/process.hpp) | OS child PID/FD 소유, spawn/stop/reap |
| ProcessRuntime | [kcf::ProcessRuntime](../kcf/include/kcf/process/process_runtime.hpp) | process 내부 lifecycle/scheduler/supervision worker |
| ProcessElement | [인터페이스](../kcf/include/kcf/process/process_element.hpp) | 사용자 Setup/Loop/Shutdown |
| Element / Component | Application 설계 단위 | 기능 process / 그 안의 Driver·Algorithm·Interface |

Supervisor는 Component 함수를 호출하거나 사용자 payload를 중계하지 않습니다.
Topic/Parameter/Service의 데이터 경로는 [기능 문서](03_IPC_AND_FEATURES.md)에 설명합니다.

## Runtime contract

[IMPLEMENTED] [Run 구현](../kcf/src/process_runtime.cpp)의 반환값과 순서:

| 사건 | 상태·정리·반환 |
| --- | --- |
| Initialize/StartSupervision 실패 | ERROR, Finalize; Setup 미호출이므로 Element Shutdown 미호출 |
| Setup 호출 시작 후 nonzero | ERROR → Shutdown 정확히 1회 → Finalize → 오류 반환 |
| Setup exception | -EFAULT; 이미 latch된 Supervisor loss가 우선할 수 있음 |
| Setup 성공 | RUNNING → 주기 Loop |
| Loop 0 | 정상 cycle heartbeat 증가; scheduler 진행 |
| Loop nonzero | 실패 cycle heartbeat 증가 없음; ERROR → Shutdown → nonzero 반환 |
| Loop exception | -EFAULT; 앞서 latch된 loss 우선 |
| SIGINT/SIGTERM/RequestStop | STOPPING → Shutdown → STOPPED, 통상 0 |
| Shutdown exception | 이전 오류 없으면 -EFAULT/ERROR; 이전 Setup/Loop/loss 오류는 보존 |
| Supervisor loss | 먼저 latch되면 ERROR; cleanup은 Run thread가 수행 |

Exactly once는 **Run이 cleanup 경로까지 진행할 수 있는 lifecycle**의 계약입니다.
SIGKILL, process crash, 끝나지 않는 Setup/Loop에 대해 Shutdown 실행을 보장하지 않습니다.
기본 Loop 주기는 10 Hz이며 유효한 SetLoopFrequency 값으로 바꿉니다. 하나의 process에는
하나의 active ProcessRuntime을 사용합니다. missed scheduler tick을 무제한 catch-up하지 않습니다.

[AUTOMATED VERIFIED] Runtime lifecycle, runtime health, Supervisor loss 회귀 및 app 예제.
오류 우선순위·blocking 한계는 [02](02_RUNTIME_AND_SUPERVISION.md)에서 상세히 다룹니다.

## Standalone / Supervised

[ExecutionMode](../kcf/include/kcf/process/execution_mode.hpp)의 `DetectLaunchExecutionMode()`는
`KCF_SUPERVISION_FD` 환경 변수 **존재**로 launch intent를 판단합니다. Run 전에 호출해야 합니다.
실제 FD 유효성은 StartSupervision이 검사하고 해당 환경 변수를 소비합니다.
Standalone에는 supervision socket/worker가 없습니다. Supervised에는 FD를 통한 별도 health worker가 있습니다.
mode 감지가 Application 안전 상태 전환이나 hardware gate를 자동 구현하지는 않습니다.

## Callback / Loop concurrency

[USER REQUIREMENT / DECISION] 짧은 callback과 Loop의 실제 처리를 분리합니다.

```text
callback → lock → 작은 POD snapshot + generation/freshness 갱신 → unlock → return
Loop     → lock → local snapshot 복사 → unlock → FSM/algorithm/Driver I/O
```

[IMPLEMENTED] [PubSub](../examples/pubsub/src/subscriber_main.cpp),
[Parameter](../examples/parameter_app/src/owner_main.cpp), [Action Client](../examples/action_app/src/client_main.cpp)가 예입니다.
Timer는 atomic tick만 증가시킵니다. Service 예제의 짧은 덧셈 callback은 요청·응답 로그를 직접 출력합니다.
위 규칙을 모든 callback의 framework-enforced single executor로 해석하면 안 됩니다.

Driver I/O, sleep, 긴 계산을 callback과 공유하는 mutex 안에서 하지 않습니다. Callback이 참조하는 state는
worker join까지 살아 있어야 합니다. Shutdown은 worker 종료 후 자원을 파괴합니다.
[CURRENT LIMITATION] Framework가 사용자 mutex deadlock이나 무한 blocking을 제거하지 않습니다.
[IMPLEMENTATION ORIGIN] 사용자 concurrency 요구를 기준으로 AI-assisted implementation; 적용 범위는 예제/API 계약별로 확인합니다.
