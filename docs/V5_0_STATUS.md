# KCF Framework v5.0 Development Status

**Development status / checkpoint candidate**, 문서 기준일 2026-09-16.
대상 `/tmp/kcf-r1-introspection`, branch `feature/introspection`의 **uncommitted 작업 상태**입니다.
확인한 HEAD: `234716b716f2f7e37c806bb9430dc9a9e11f74ab`.
이는 baseline commit이며 현재 introspection/app 예제 전체를 담은 release commit이 아닙니다.
`git tag --points-at HEAD` 출력은 없었습니다. Exact release commit/tag: **TBD**.
새 tag/commit/version history를 생성하지 않았습니다. 기존 v4.x 이력 및 Tool v0.1은 별도 명명 범위입니다.

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
특정 pubsub SIGKILL/Tool Refresh 결합 검증, desktop manual/hardware/real-time 보장은 확인되지 않았습니다.

이 문서 작성은 README/docs만 추가합니다. 기존 기능·예제·Tool·mecanum·CMake·테스트 코드와 기존 개발 기록은 변경하지 않습니다.

## 문서 추적 상태

기존 `.gitignore`가 `docs/` 전체를 제외합니다. 상세 문서 9개는 파일로 존재하지만
일반 git status의 신규 파일 목록에 나타나지 않습니다. 이번 허용 범위가 README/docs이므로
.gitignore와 index는 변경하지 않았습니다. 향후 문서를 repository에 포함할 때 별도 추적 처리가 필요합니다.
