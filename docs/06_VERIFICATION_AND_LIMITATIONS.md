# 06. Verification and Limitations

## 증거 구분

이번 문서 작업에서는 코드를 변경하거나 기능 테스트를 재실행하지 않았습니다.
기존 코드/테스트/문서/최근 실제 실행 로그를 대조하고 `ctest --test-dir build -N`으로 등록 수를 확인했습니다.
수치와 PASS는 아래 실행 출처의 범위에 한정됩니다. 하드웨어·hard real-time·무한 부하 보증이 아닙니다.

## Framework: CTest 0, sequential runner 22

현재 Framework top-level CMake에는 CTest 등록이 없으며 `ctest -N` 결과는 **Total Tests: 0**입니다.
최근 Action app 작업의 `/tmp/action-app-regressions.py`가 **20개 case + legacy Service + Integration = 22개 실행 항목**을
순차 실행했고 최종 `FAILED: []`였습니다. 각 항목에는 여러 내부 assertion/scenario가 포함됩니다.
22개 CTest test가 있다는 뜻이 아닙니다.

| # | runner label | 실제 실행 대상 (`build/` 기준, Python은 source 기준) |
| --- | --- | --- |
| 1 | r41 | examples/introspection/kcf_shm_incarnation_test |
| 2 | r5 | examples/introspection/kcf_service_introspection_test |
| 3 | r4 | examples/introspection/kcf_dynamic_access_test |
| 4 | r3 | examples/introspection/kcf_type_descriptor_test |
| 5 | r2b | examples/introspection/kcf_supervisor_discovery_test |
| 6 | named_cli | 위 executable --cli build/bringup/kcf_bringup |
| 7 | r2a | examples/introspection/kcf_endpoint_registry_test |
| 8 | discovery | examples/introspection/kcf_runtime_discovery_test |
| 9 | lifecycle | examples/supervisor/kcf_runtime_lifecycle_test |
| 10 | runtime_health | python3 examples/supervisor/runtime_health_checks.py build |
| 11 | supervisor_loss | python3 examples/supervisor/supervisor_loss_checks.py build |
| 12 | reset | python3 examples/supervisor/reset_checks.py build |
| 13 | system_status | examples/supervisor/kcf_system_status_recovery_test |
| 14 | topic_recovery | examples/recovery/kcf_topic_recovery_test |
| 15 | parameter_recovery | examples/recovery/kcf_parameter_recovery_test |
| 16 | incomplete_topic | examples/recovery/kcf_incomplete_recovery_test topic |
| 17 | incomplete_parameter | examples/recovery/kcf_incomplete_recovery_test parameter |
| 18 | runtime_protocol | examples/supervisor/kcf_runtime_supervision_test + kcf_supervisor_runtime_test executable path |
| 19 | application_scope | examples/supervisor/kcf_application_scope_test |
| 20 | timer | examples/timer/kcf_timer_test |
| 21 | legacy_service | kcf_service_server port + kcf_service_client port 1000, 정상 server 종료 |
| 22 | integration | kcf_integration_backend + kcf_integration_client, 정상 backend 종료 |

현재 확인한 결과: **22/22 PASS**. 로그 디렉터리: `/tmp/action-app-regression/`.
마지막 두 항목은 concurrent server/client pair를 runner가 관리합니다. `python3 /tmp/action-app-regressions.py`는
현재 환경의 재현 명령이나 runner는 repository에 포함되지 않은 임시 artifact입니다.
배포된 checkout에 그 파일이 존재한다고 보장하지 않습니다. 개별 source/target은
[supervisor](../examples/supervisor/), [introspection](../examples/introspection/), [recovery](../examples/recovery/),
[integration](../examples/integration/)에서 확인할 수 있습니다.

## Application-level 실제 검증

| 예제 | 확인된 결과 | 실제 근거 artifact |
| --- | --- | --- |
| PubSub | standalone/supervised, matching samples, 양 endpoint, Echo 5 fields, 정상 cleanup PASS | `/tmp/pubsub-ui-run.log`, `/tmp/pubsub-supervised.log` |
| Parameter | initial Get, Apply/re-read/owner readback/watcher, Revert, invalid input, stale, restart PASS | `/tmp/parameter-ui-run.log`, `/tmp/parameter-standalone.log`, `/tmp/parameter-supervised.log` |
| Service | form, signed/zero, validation, duplicate guard, timeout, old identity handler 미호출, restart PASS | `/tmp/service-ui-run.log`, `/tmp/service-standalone.log`, `/tmp/service-supervised.log` |
| Timer | period, monotonic ticks, SIGTERM/SIGINT, post-Shutdown no ticks, restart PASS | `/tmp/timer-app-runtime.log`, `/tmp/timer-app-regression/timer.log` |
| Action | Success/Cancel, invalid goal, bounded server-exit failure, Supervisor ERROR, restart, legacy Action PASS | `/tmp/action-app-tests.log`의 완료 항목 + `/tmp/action-app-tail.log`, `/tmp/action-supervised.log`, `/tmp/action-crash.log` |

Action의 최초 임시 테스트에는 global discovery count 가정, startup barrier 이전 kill,
Supervisor 오류 종료 코드를 0으로 기대한 test 오류가 있었습니다. 마지막 tail 검증은 RUNNING 이후 kill과
오류 종료를 기대해 통과했습니다. 앞선 traceback을 숨기거나 전체 로그를 무조건 성공으로 간주하지 않습니다.
Timer discovery 임시 테스트도 해당 Runtime 범위로 보정했습니다. 제품 코드 수정으로 우회하지 않았습니다.
PubSub 작업 당시 scope test 최초 1회 timeout 후 재실행 PASS가 있었으며 원인은 확정하지 않았습니다.
최근 Action 단계의 22개 회귀는 전체 통과했습니다.

Timer 숫자는 측정 구간별 값입니다. Timer app 로그 간격은 500.0/500.1/489.9 ms이고,
최근 Action 단계의 low-level Timer 회귀 2 Hz callback 평균은 **500.025 ms**입니다.
다른 단계의 499.989 ms 측정과 혼합하지 않습니다.

## Tool: 기존 문서에서 확인한 과거 결과

[KT-8](/home/kssvm/workspace/kcf/kcf_tools/docs/KT8_GENERIC_CROSS_APPLICATION_VALIDATION.md) 및
[release notes](/home/kssvm/workspace/kcf/kcf_tools/docs/V0_1_RELEASE_NOTES.md), 기록일 2026-09-15:
KT-8 **1/1**, Tool regression **10/10**, KCF-less GUI **3/3**, KCF-less backend **1/1 PASS**.
이는 이번 문서 작업의 재실행 결과가 아닙니다. 당시 Framework는 runner 19 + 추가 2 항목이며
최근 22개 runner와 구성/시점이 다릅니다.

최근 app 예제의 Tool 검증은 기존 UI/backend source를 수정 없이 링크한 임시 Qt offscreen harness의 자동 입력·버튼 조작입니다.
기존 v0.1 binary 실행도 별도 확인했지만, 기존 binary에 대한 모든 클릭을 OS automation으로 수행했다는 뜻은 아닙니다.

수동 검증과의 구분:

- Pub/Sub Echo, Parameter Apply, Service Call: 실제 backend를 사용한 **offscreen 자동 GUI 검증**.
- Standalone discovery, app restart, Action lifecycle: 실제 process를 실행한 **자동 integration/로그 검증**.
- SIGKILL → Supervisor ERROR: Action app 및 기존 fixture의 **failure injection 검증**.
- 특정 pubsub subscriber SIGKILL + healthy publisher 지속 + Tool Refresh의 결합: 현재 로그에서 **확인되지 않음**.
- 실제 데스크톱 수동 조작, 실제 hardware safety: **이번 기록에서 검증되지 않음**.

## Known Limitations

| ID | Area | Limitation | Impact | Current Mitigation | Future Option | Status |
| --- | --- | --- | --- | --- | --- | --- |
| L-001 | SharedChannel | dead-reader pin 잔존 | slot 제약/-EAGAIN | bounded 반환 처리, controlled owner recovery | reader liveness 회수 | Open |
| L-002 | SharedParameter | synchronous locking | reader/worker 정지가 접근을 block 가능 | 짧은 작업·dead-owner robust recovery | bounded isolation | Open |
| L-003 | Service | manual UDP port | bind 충돌 | Application이 서로 다른 port 지정 | 명시적 관리 도구 | Retained |
| L-004 | Service identity | 검증과 송신 non-atomic, wire registration_id 없음 | 마지막 검사 직후 재등록 race | stale 사전 검사/명시적 재선택 | protocol generation token | Open |
| L-005 | Service timeout | timeout != 미실행 | 무분별한 재호출 위험 | 오류 표기·Tool 추가 retry 없음 | Application idempotency 설계 | Contract |
| L-006 | Dynamic access | stable descriptor 필요 | legacy/unknown type generic 접근 제한 | explicit TypeDescriptorTraits | schema 확장 | Contract |
| L-007 | Timer | introspection endpoint 없음 | Tool Timer 표시 불가 | Runtime만 발견 | 별도 요구 검토 | Intentional |
| L-008 | Action | Tool 전용 UI/model 없음 | Goal/Result GUI 조작 불가 | app client 사용 | Action model | Deferred |
| L-009 | Tool discovery | manual Refresh | topology/state 자동 갱신 아님 | Refresh/reselect | 관찰 lifecycle | Retained |
| L-010 | Topic Echo | polling, Dynamic Monitor 없음 | sample 누락·poll latency | persistent 100 ms reader | callback monitor | Retained |
| L-011 | Introspection | fixed capacities | metadata 전체 노출 못 할 수 있음 | 규모/등록 결과 관찰 | capacity 정책 | Open |
| L-012 | ABI | 호환 Framework/native layout 필요 | 다른 build/ABI 간 통신 보장 없음 | 동일 호환 revision rebuild/restart | portable serialization | Contract |
| L-013 | SHM | format 3 호환 검사 | 이전 format 자동 migration 없음 | owner lifecycle과 호환 build 관리 | migration 정책 | Contract |
| L-014 | Service | protocol 2 호환 검사 | 이전 protocol과 호환 보장 없음 | client/server 함께 rebuild | version negotiation | Contract |
| L-015 | Namespace | global logical SHM ownership | 같은 Topic의 복수 owner 충돌 | explicit prefix/port별 이름 | namespace 정책 | Open |
| L-016 | Action | single active goal 중심 | queue/concurrent/preemption 미지원 | 순차 scenario | 정책 확장 | Retained |
| L-017 | SystemStatus | 외부 observer에 Application identity 필요 | 이름만으로 자동 연결 안 됨 | OpenForApplication(pid,start_ticks) | explicit resolver | Contract |

L-011 현재 값: endpoints/runtime **128**, types/runtime **64**, services/runtime **64**,
Supervisor metadata elements **64**, descriptor fields **128**. 이는 introspection 용량이며
그대로 process 실행 수의 hard limit으로 일반화하지 않습니다.
근거: [introspection detail](../kcf/include/kcf/introspection/detail/),
[SupervisorInfo](../kcf/include/kcf/introspection/supervisor_info.hpp), [TypeDescriptor](../kcf/include/kcf/introspection/type_descriptor.hpp).
기능별 구현 근거는 [03](03_IPC_AND_FEATURES.md), identity race는 [04](04_INTROSPECTION_AND_TOOL.md) 참조.

추가 한계: blocked Setup/Loop의 강제 cleanup 불가, registry 조회의 synchronous 접근,
죽은 process의 stale metadata가 남을 수 있음, 인증/remote security boundary 미제공.
/tmp artifact는 영구 보존을 보장하지 않으므로 장기 release 증거로 필요하면 별도 보존 절차가 필요합니다.
