# 06. Verification and Limitations

## 증거 구분

### Historical temporary verification artifacts

아래 `/tmp/...` script/log 경로는 당시 개발 검증 근거이며 repository의 영구 artifact가 아닙니다.
These paths were used during development verification and are not part of the repository or reproducible release artifact.
해당 파일의 존재는 v5.0 사용/빌드 조건이 아닙니다. 사용자 수동 검증은 별도 출처로 기록합니다.


기존 문서 정리 단계에서는 코드를 변경하거나 기능 테스트를 재실행하지 않았습니다.
기존 v5.0 문서 정리 당시에는 코드/테스트/문서/로그 대조와 `ctest --test-dir build -N` 등록 수 확인을 수행했습니다.
이전 Topic Queue 문서 반영에서는 아래 구현 완료 보고를 기록했으며 빌드·테스트·CTest 조회를 재실행하지 않았습니다.
수치와 PASS는 아래 실행 출처의 범위에 한정됩니다. 하드웨어·hard real-time·무한 부하 보증이 아닙니다.

## 기존 v5.0 Automated / Regression Verification

[AUTOMATED VERIFIED] Framework sequential runner 22/22 PASS.
Introspection R1~R5, Runtime/Supervisor/recovery/timer/integration을 포함합니다.

### Framework: CTest 0, sequential runner 22

기존 v5.0 확인 당시 Framework top-level CMake의 `ctest -N` 결과는 **Total Tests: 0**이었습니다.
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

기존 v5.0 기록의 결과: **22/22 PASS**. 로그 디렉터리: `/tmp/action-app-regression/`.
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

<a id="topic-queue-verification"></a>

## Topic Queue — v5.1 확정 이전 구현 검증 보고

[REPORTED RESULT] 출처는 v5.1 확정 이전의 **Topic Queue 구현 완료 보고**입니다.
아래 PASS는 해당 구현 작업에서 보고된 결과이며, **당시 문서 반영 중 빌드나 테스트를 재실행하지 않았습니다**.
기존 v5.0의 22개 회귀·GUI 수동 검증 및 아래 v5.1 최종 실행 결과와 구분합니다. 당시에는 버전 확정 전의 개발 변경으로 기록했습니다.

| 검증 | 보고된 결과 | 범위 |
| --- | --- | --- |
| 전체 빌드 | PASS | C++17 전체 build |
| 신규 Queue 필수 검증 | 10개 영역 PASS | `examples/recovery/kcf_topic_queue_test` 및 관련 회귀; 아래 영역 구분 |
| 기존 회귀 | 19개 항목 PASS | `examples/introspection/r41_regressions.py`의 17개 case + legacy Service + Integration |
| 기존 kcf_tools Backend/Topic Echo | PASS | Tool 소스 수정 없이 새 Core와 연결한 `kcf_tool_real_backend` 테스트; GUI 수동 재검증을 의미하지 않음 |

Queue 필수 검증 영역은 다음과 같습니다. 10개 독립 CTest 등록 수라는 뜻은 아닙니다.

1. Depth=1 Snapshot 회귀.
2. Depth=N 메시지 보관 순서.
3. 서로 다른 속도의 Subscriber와 독립 Cursor.
4. Overflow 이후 정확한 누락 개수.
5. Reader 복사 중 Writer 덮어쓰기 방지.
6. 실패한 Publish의 Sequence 및 Queue 상태 유지.
7. Reader 비정상 종료와 잔존 pin 한계.
8. Producer 종료·재시작, SHM 재생성 및 명시적 재연결.
9. Depth와 무관한 DynamicTopicReader 최신값 조회.
10. `valid=false` Payload의 변경 없는 전달.

19개 회귀는 위 기존 v5.0 표의 **1~17번, 21~22번**에 해당합니다.
Runtime Health, Supervisor Loss, Reset, Topic/Parameter 복구 및 incomplete storage 검사를 포함하며,
기존 22개 전체를 이번 Queue 변경으로 다시 검증했다고 표현하지 않습니다.
구현 검증 중 구형 SHM Layout/Format을 직접 사용하던 테스트가 실패하여 테스트 매핑·Format 지정만 보정한 뒤
관련 검증을 통과한 것으로 보고됐습니다. 검증 조건이나 production 복구 기능을 제거한 결과가 아닙니다.

보고된 실행 근거: repository의 [Queue 테스트](../examples/recovery/src/topic_queue_test.cpp),
[기존 회귀 runner](../examples/introspection/r41_regressions.py), 개발 환경의 `build/r41-validation/`,
`/tmp/queue-final-build.log`, `/tmp/queue-reset-build.log`, `/tmp/kcf-topic-queue-tool-validation/`.
로그·빌드 경로는 영구 artifact나 현재 존재 보장이 아닙니다.

<a id="v51-final-verification"></a>

## v5.1 최종 실행 검증

[AUTOMATED VERIFIED — FINAL RUN] **2026-09-19**, `dev`의 v5.1 작업 트리에서 실제로 실행했습니다.
CMake project version은 5.1.0이며 새로운 `build/v5.1` 디렉터리에서 C++17 Debug 전체 빌드를 수행했습니다.
이 절은 위의 기존 v5.0 기록 및 Queue 구현 완료 보고를 복사한 결과가 아닙니다.

| 검증 | 이번 실행 결과 | 대상 |
| --- | --- | --- |
| Configure / 전체 빌드 | PASS | C++17 Debug, 기존 example 포함 |
| Topic Queue | 필수 10개 영역 PASS | `kcf_topic_queue_test`; 영역 정의는 위 Queue 보고 목록과 동일 |
| 기존 회귀 runner | 19/19 PASS | `r41_regressions.py`: 17개 case + legacy Service + Integration, `FAILED: []` |
| 추가 Framework 회귀 | 3/3 PASS | Runtime protocol, Application scope, Timer |
| Framework 회귀 합계 | 22/22 PASS | 이번 19개 runner와 추가 3개 실행의 합계; CTest 등록 개수를 의미하지 않음 |
| 기존 kcf_tools Backend/Topic Echo | 1/1 PASS | Tool 소스 수정 없이 새 Core로 별도 빌드한 `kcf_tool_real_backend` |

Framework 실행 명령:

```sh
cmake -S . -B build/v5.1 -DCMAKE_BUILD_TYPE=Debug
cmake --build build/v5.1 -j4
./build/v5.1/examples/recovery/kcf_topic_queue_test
python3 examples/introspection/r41_regressions.py build/v5.1
./build/v5.1/examples/supervisor/kcf_runtime_supervision_test ./build/v5.1/examples/supervisor/kcf_supervisor_runtime_test
./build/v5.1/examples/supervisor/kcf_application_scope_test
./build/v5.1/examples/timer/kcf_timer_test
```

Tool은 기존 외부 `kcf_tools` 저장소를 사용하고 `KCF_SOURCE_DIR`를 현재 KCF 저장소로 지정했습니다.
`KCF_TOOL_BUILD_GUI=OFF`, `KCF_TOOL_BUILD_TESTS=ON`, Debug로 `/tmp/kcf-v51-tool-validation`에
`kcf_tool_real_backend`와 `kcf_tool_real_fixture`를 빌드한 뒤 다음을 실행했습니다.

```sh
ctest --test-dir /tmp/kcf-v51-tool-validation -R '^kcf_tool_real_backend$' --output-on-failure
```

이번 실행 로그: `/tmp/kcf-v51-build.log`, `/tmp/kcf-v51-queue.log`, `/tmp/kcf-v51-regressions.log`,
`build/v5.1/r41-validation/` (추가 3개 로그 포함), `/tmp/kcf-v51-tool-build.log`, `/tmp/kcf-v51-tool-test.log`.
이 경로는 개발 환경의 임시 산출물이며 repository에 포함하거나 영구 보존을 보장하지 않습니다.
이 테스트는 GUI 수동 재검증, 실제 장치 검증, hard real-time 또는 dead-reader pin 자동 회수를 보장하지 않습니다.
버전 확정에 따른 Git commit/push/tag 생성은 수행하지 않았습니다.

## Manual GUI Verification

[MANUALLY VERIFIED] 아래는 기존 v5.0 보정 요청에서 사용자가 제공한 **직접 GUI/process 검증 이력**이다.
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
| L-013 | SHM | Topic format 4 / Parameter format 3 호환 검사 | 이전 format 자동 migration 없음 | owner lifecycle과 호환 build 관리 | migration 정책 | Contract |
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

Topic Queue 추가 계약·한계: Topic당 단일 Publisher/쓰기 thread, 논리 Entry당 3개 Payload Slot의 메모리 비용,
KEEP_LAST Overflow에 따른 유실 가능성을 유지합니다. Dead-reader pin은 자동 회수하지 않으며 안전한 Slot 부족은
`-EAGAIN`입니다. Typed Subscriber는 기존 mapping에서 새 SHM으로 자동 이동하지 않으므로 명시적 Close/Create
(SharedChannel은 Close/Open)가 필요합니다. DynamicTopicReader는 재생성을 `-ESTALE`로 알립니다.
Transport Sequence 최대값에서는 Publish가 `-EOVERFLOW`이며, Payload Timestamp/Validity의 의미와 판단은 Application 책임입니다.
이전 Topic Format 3과 Format 4는 바이너리 호환되지 않아 참여 바이너리 재빌드가 필요하고 자동 삭제·변환은 하지 않습니다.
