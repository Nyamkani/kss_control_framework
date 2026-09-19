# 06. Verification and Limitations

## 증거 구분

### Historical temporary verification artifacts

아래 `/tmp/...` script/log 경로는 당시 개발 검증 근거이며 repository의 영구 artifact가 아닙니다.
These paths were used during development verification and are not part of the repository or reproducible release artifact.
해당 파일의 존재는 v5.0 사용/빌드 조건이 아닙니다. 사용자 수동 검증은 별도 출처로 기록합니다.


이번 문서 작업에서는 코드를 변경하거나 기능 테스트를 재실행하지 않았습니다.
기존 코드/테스트/문서/최근 실제 실행 로그를 대조하고 `ctest --test-dir build -N`으로 등록 수를 확인했습니다.
수치와 PASS는 아래 실행 출처의 범위에 한정됩니다. 하드웨어·hard real-time·무한 부하 보증이 아닙니다.

## Automated / Regression Verification

[AUTOMATED VERIFIED] Framework sequential runner 22/22 PASS.
Introspection R1~R5, Runtime/Supervisor/recovery/timer/integration을 포함합니다.

### Framework: CTest 0, sequential runner 22

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
당시 개발 환경에서 사용한 실행 명령이며 runner는 repository에 포함되지 않은 임시 artifact입니다.
배포된 checkout에 그 파일이 존재한다고 보장하지 않습니다. 개별 source/target은
[supervisor](../examples/supervisor/), [introspection](../examples/introspection/), [recovery](../examples/recovery/),
[integration](../examples/integration/)에서 확인할 수 있습니다.

### Application-level Automated Verification

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

### Tool: 기존 문서에서 확인한 과거 결과

KT-8 (`Nyamkani/kcf_tools/docs/KT8_GENERIC_CROSS_APPLICATION_VALIDATION.md`) 및
release notes (`Nyamkani/kcf_tools/docs/V0_1_RELEASE_NOTES.md`), 기록일 2026-09-15:
KT-8 **1/1**, Tool regression **10/10**, KCF-less GUI **3/3**, KCF-less backend **1/1 PASS**.
이는 이번 문서 작업의 재실행 결과가 아닙니다. 당시 Framework는 runner 19 + 추가 2 항목이며
최근 22개 runner와 구성/시점이 다릅니다.

최근 app 예제의 Tool 검증은 기존 UI/backend source를 수정 없이 링크한 임시 Qt offscreen harness의 자동 입력·버튼 조작입니다.
기존 v0.1 binary 실행도 별도 확인했지만, 기존 binary에 대한 모든 클릭을 OS automation으로 수행했다는 뜻은 아닙니다.

## Manual GUI Verification

[MANUALLY VERIFIED] 아래는 이번 보정 요청으로 사용자가 제공한 **직접 GUI/process 검증 이력**이다.
자동 regression fixture 결과나 이번 문서 수정 중 재실행한 결과로 분류하지 않는다.

### Pub/Sub

- Supervised `pubsub_example` RUNNING 및 publisher/subscriber 2개 Element 확인.
- Topic Echo의 5개 field와 실제 증가 값 확인.
- Subscriber SIGKILL 후 Supervisor RUNNING → ERROR, healthy publisher 계속 실행 확인.
- Tool Refresh 전 기존 snapshot 유지, Refresh 후 failed subscriber 제거 및 publisher만 남는 topology 확인.
- Bringup 전체 종료·재실행 후 새 Supervisor/child PID/start_ticks generation 확인.
- Standalone publisher/subscriber discovery 확인.

Result: **Manual GUI / process failure / restart PASS**.
이 결합 scenario는 manual integration verification이며 automated regression fixture가 아니다.

### Parameter

`parameter_example` RUNNING, `/example/config` OWNER, field descriptor 표시,
Edit/Apply, 변경값 재조회, Owner readback, Revert를 직접 확인했다.
Result: **Manual GUI PASS**.

### Service

`service_example` RUNNING과 `/example/add` 표시를 확인하고 request a=10, b=25를 입력했다.
Response result=35, accepted=true를 확인했다. Result: **Manual GUI PASS**.

## Manual Console Verification

[MANUALLY VERIFIED] 사용자 직접 console/runtime 확인 이력이다.

### Timer

Standalone/supervised 500 ms periodic output, tick 증가, shutdown 후 tick 중단 확인.
Result: **Manual console PASS**. 정밀한 hard real-time 보장을 의미하지 않는다.

### Action

- Success: target=10, IDLE → ACCEPTED → RUNNING → SUCCEEDED,
  feedback sequence/count 1~10, progress 0.1 → 1.0, final=10, result_code=0.
- Cancel: target=50, feedback 진행 후 약 500 ms에 Cancel, CANCELED,
  result_code=-ECANCELED 및 cancel 이후 feedback 중단.
- Client는 두 scenario 완료 후 idle/RUNNING 유지.

Result: **Manual console PASS**. 취소 final count는 타이밍에 따라 달라진다.
Hardware safety / hard real-time / 실제 Device I/O는 이번 v5.0 Framework 검증 범위가 아니다.

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
