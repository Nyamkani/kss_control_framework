# KCF Framework v5.0 Development Status

> Historical v5.0 checkpoint 기록입니다. 아래의 현재/이번 표기는 당시 기준입니다.
> Topic Queue와 현재 버전은 [v5.1 상태](V5_1_STATUS.md)를 참조하세요.


문서 기준일: 2026-09-16.

KCF Framework v5.0의 현재 development checkpoint는 `feature/introspection` branch에 반영되어 있다.
문서 정리 전 확인한 implementation checkpoint:
`cba15f55a7fca3b22a7c5584fc063c7b51b93dc9`.
이 commit hash는 문서 수정 이후 branch HEAD와 달라질 수 있으므로 정식 release identity로 사용하지 않는다.
Exact release tag: **TBD**.

현재 v5.0 branch에는 Process / ProcessRuntime / Supervisor, Topic / Parameter / Service / Timer / Action,
R1 / R2A / R2B / R2B.1 / R3 / R4 / R4.1 / R5, application-level examples와 v5.0 documentation set이 포함된다.
기존 v4.x version history와 kcf_tools v0.1은 서로 다른 version namespace이다.

## 구현 snapshot

| 영역 | 현재 상태 |
| --- | --- |
| Process | fork/exec, supervision socketpair, stop/force stop/reap |
| Runtime | Setup/Loop/Shutdown, fatal error, heartbeat, Supervisor loss |
| Supervisor | startup barrier, health, ERROR latch, 명시적 Reset |
| Topic | SHM triple-buffer latest-value |
| Parameter | Get/Set/current value/watcher |
| Service | localhost UDP, manual port, protocol type/layout checks |
| Timer | local worker, Stop/join |
| Action | Service + status Topic, single active goal lifecycle |
| Introspection | R1, R2A, R2B, R2B.1, R3, R4, R4.1, R5 구현 확인 |

Compatibility: **SHM format 3**, **Service protocol 2**, descriptor protocol 1,
Runtime supervision protocol 1. Native ABI와 호환 Framework revision으로 rebuild/restart해야 합니다.
[코드 근거와 한계](03_IPC_AND_FEATURES.md)를 함께 확인합니다.

## Examples / Tool

[pubsub](../examples/pubsub/README.md), [parameter_app](../examples/parameter_app/README.md),
[service_app](../examples/service_app/README.md), [timer_app](../examples/timer_app/README.md),
[action_app](../examples/action_app/README.md)의 구현과 실제 실행 검증이 있습니다.
기존 low-level 예제는 보존했습니다.

Tool: generic Application discovery, Topic Echo, Parameter Edit, Service Call.
Action UI: **NOT IMPLEMENTED**. Timer endpoint: **NOT APPLICABLE**.
Discovery/state는 manual Refresh, Echo data는 periodic polling입니다. Tool용 Reset/Launch manager는 제공 기능으로 선언하지 않습니다.

## Tests

최근 Framework runner **22/22 PASS**, Framework CTest 등록 **0**.
5개 app 예제의 정상 경로와 문서화된 failure/restart 경로 PASS.
기존 low-level Action Success/Cancel PASS. Tool 문서 기준 KT-8 1/1, Tool regression 10/10,
KCF-less GUI 3/3, backend 1/1 PASS는 과거 기록이며 이번 문서 작업에서 재실행하지 않았습니다.

[검증 범위·artifact·실패 이력·17개 known limitations](06_VERIFICATION_AND_LIMITATIONS.md)를 release 판단의 일부로 함께 봐야 합니다.
[MANUALLY VERIFIED] 사용자 직접 검증으로 PubSub GUI·subscriber SIGKILL·healthy publisher 지속·Refresh·restart,
Parameter Edit/Apply/Revert, Service Call, Timer 및 Action console lifecycle PASS를 확인했습니다.
Hardware safety / hard real-time / 실제 Device I/O는 이번 Framework 검증 범위가 아닙니다.

이번 보정은 README/docs 문구만 수정하며 기능·예제·Tool·CMake·테스트 코드는 변경하지 않습니다.

## Repository State

v5.0 문서 세트는 현재 repository의 `docs/`에 추적되고 있다.
Application reference implementation인 Mecanum은 현재 v5.0 Framework branch 범위에서 제외되어 있으며,
Framework와 Application 개발 이력을 분리하여 관리한다.
