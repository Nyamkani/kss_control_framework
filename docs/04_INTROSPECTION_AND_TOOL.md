# 04. Introspection and Tool

[USER REQUIREMENT / DECISION] 새로운 Application/Element를 추가해도 Tool source 수정 없이 구조를 발견하고
Topic/Parameter/Service를 사용할 수 있어야 합니다.
[IMPLEMENTATION ORIGIN] 해당 요구에 대한 R1–R5 metadata/dynamic API는 AI-assisted implementation으로 기록합니다.
개별 세부 설계의 최초 제안·승인 기록이 없으면 사용자의 최초 설계로 소급하지 않습니다.

## Control Plane / Data Plane

Control Plane: Runtime self-description, Supervisor membership, Tool discovery.
Data Plane: Topic SHM direct, Parameter SHM direct, Service UDP direct.
Supervisor는 broker가 아닙니다. IntrospectionClient는 observational API이며 registry가 인증/권한 경계도 아닙니다.
[공개 API](../kcf/include/kcf/introspection/introspection_client.hpp)의 목록 조회는 best-effort이며 atomic system snapshot이 아닙니다.

Identity: Runtime/Supervisor는 **PID + Linux process_start_ticks**, endpoint는 이에 **registration_id**를 더합니다.
이 identity는 현재 부팅된 시스템 범위이며 영구 ID가 아닙니다. registration_id 숫자만 global uniqueness를 가정하지 않습니다.
Application 이름은 표시 이름이며 instance identity가 아닙니다.

## 단계별 구현과 출처

| Stage | Problem / Requirement origin | Implemented solution | Verification | Remaining limitation |
| --- | --- | --- | --- | --- |
| R1 | User: runtime generic discovery | RuntimeInfo + registry/ListRuntimes | runtime_discovery_test | 전이 snapshot, live heartbeat/history 아님 |
| R2A | User: endpoint generic discovery | Publisher/Subscriber/Parameter registry | endpoint_registry_test | fixed capacity, metadata best-effort |
| R2B | User: Application membership | SupervisorInfo, Element membership | supervisor_discovery_test/named CLI | 이름으로 identity 대체 불가 |
| R2B.1 | User: multiple Applications; KT-7 collision | instance-scoped SystemStatus | application_scope_test, KT-8 | 외부 observer는 instance identity 필요 |
| R3 | User generic Tool requirement → implementation design | stable flat TypeDescriptor | type_descriptor_test | arbitrary reflection/variable container 없음 |
| R4 | generic payload access | DynamicTopicReader/DynamicParameterClient | dynamic_access_test | descriptor/ABI 필요, synchronous access |
| R4.1 | KT-4 recreate 검증 문제 → corrective implementation | SHM device/inode 재검증 | shm_incarnation_test, KT-4.1 | 시점 검사; atomic namespace transaction 아님 |
| R5 | generic Service Call 요구 | ServiceInfo + DynamicServiceClient, protocol type/layout | service_introspection_test | validation/send registration race |

[관련 테스트 소스](../examples/introspection/src/), [dynamic API](../kcf/include/kcf/dynamic/).
과거 R1 문서의 Deferred 번호는 이 표를 대체하지 않습니다.

## R2B.1 corrective history

1. User requirement: 여러 Application이 독립적으로 동작해야 함.
2. [KT-7 기록](/home/kssvm/workspace/kcf/kcf_tools/docs/KT7_APPLICATION_ELEMENT_EXPLORER.md):
   두 번째 Bringup의 global SystemStatus Create가 -EEXIST; 당시 실제 동시 실행은 FAIL.
3. 이번 사용자 제공 이력: AI/Codex가 Application-instance scope를 corrective design으로 제안·구현하고 사용자 승인/후속 검증으로 수용.
   별도 최초 승인 commit artifact는 현재 확인되지 않음.
4. [현재 코드](../kcf/include/kcf/system/system_status_scope.hpp): Supervisor PID/start_ticks scope,
   [Bringup](../bringup/src/bringup.cpp)이 자식에 전달. Reset에는 같은 scope, 새 child generation.
5. [현재 scope test](../examples/supervisor/src/application_scope_test.cpp) 및
   [KT-8 기록](/home/kssvm/workspace/kcf/kcf_tools/docs/KT8_GENERIC_CROSS_APPLICATION_VALIDATION.md):
   two supervisors / same application_name / state isolation PASS.

global SystemStatus collision은 해결된 historical failure입니다. 일반 Topic namespace 자동 격리까지 해결했다는 뜻은 아닙니다.

## R3 descriptor

[TypeDescriptor](../kcf/include/kcf/introspection/type_descriptor.hpp)는 canonical name의 FNV-1a 64-bit type_id,
size/alignment, field offset/kind/array_count를 담습니다. bool, fixed-width integers, float32/64, primitive fixed array를 지원합니다.
Nested POD는 사용자가 offset을 합해 flat field로 명시할 수 있으나 임의 C++ reflection은 없습니다.
해시는 cryptographic/collision-free ID가 아닙니다. 비호환 contract에는 새 canonical name을 사용합니다.
`protocol_version=1`은 descriptor protocol이며 별도 Application schema version 필드는 아닙니다.

## Tool v0.1 범위

[Tool release notes](/home/kssvm/workspace/kcf/kcf_tools/docs/V0_1_RELEASE_NOTES.md)는 별도 repository의 문서입니다.
위 절대 경로 링크는 이 개발 환경의 근거 위치이며 다른 checkout에서는 해당 Tool docs를 별도로 확인해야 합니다.

| 지원 | 동작 |
| --- | --- |
| Application/Element explorer | 수동 Refresh로 topology/state 갱신 |
| Topic Echo | persistent reader + 약 100 ms periodic polling |
| Parameter Viewer/Editor | Get, validation, Apply/Revert, re-read |
| Service Call | descriptor form, 비동기 단일 Call, response 표시 |

미지원: Action UI, Timer endpoint, Graph, Launch manager, Plot/history, Dynamic callback Monitor,
Parameter auto-watch, remote access. 화면의 Launch tab 존재를 실제 process launch manager 지원으로 해석하지 않습니다.
Topic Echo의 주기적 데이터 조회와 Application discovery의 수동 Refresh는 다릅니다.
Reset은 Framework API/fixture에서 검증했으며 Tool 사용자용 Reset 기능으로 설명하지 않습니다.

[VERIFIED] 최근 app 예제는 수정 없는 기존 UI/backend의 offscreen 자동 입력/클릭으로 검증했습니다.
기존 Tool binary 실행도 별도로 확인했습니다. 실제 데스크톱 사용자의 수동 조작/시각 검토와 동일한 증거는 아닙니다.
