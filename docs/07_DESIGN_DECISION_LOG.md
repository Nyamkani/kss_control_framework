# 07. Design Decision Log

가벼운 ADR 기록입니다. **Date/Stage는 아래 단계명**으로 기록하며 확인되지 않은 최초 결정 날짜를 만들지 않습니다.
문서 정리일은 2026-09-16입니다. 출처 분리 기준은 [00](00_PROJECT_SCOPE_AND_DECISIONS.md), 검증 상세는 [06](06_VERIFICATION_AND_LIMITATIONS.md) 참조.
User 제공 승인 이력과 코드 존재를 별개 증거로 취급합니다. “User-approved”에 대해 독립 승인 artifact가 없는 경우 그 사실을 유지합니다.

## D-001 — Process-based architecture

| 항목 | 기록 |
| --- | --- |
| ID | D-001 |
| Date/Stage | 현재 checkpoint; 최초 날짜 미확인 |
| Decision Source | User |
| Context | 기능별 독립 개발과 유지보수 |
| Decision | 기능을 process로 분리 |
| Reason | 장애·실행 경계 명시 |
| Implementation | Process/ProcessRuntime |
| Verification | spawn/lifecycle 및 app 예제; 증거 종류는 [검증 문서](06_VERIFICATION_AND_LIMITATIONS.md) |
| Current Status | Retained |
| Future Revisit Trigger | process 비용이 실제 요구를 넘을 때; 현재 결정/구현 약속 아님 |

## D-002 — Element independent executable

| 항목 | 기록 |
| --- | --- |
| ID | D-002 |
| Date/Stage | 현재 checkpoint; 최초 날짜 미확인 |
| Decision Source | User |
| Context | 각 기능의 독립 검증 |
| Decision | Element별 main/executable |
| Reason | Bringup 없이도 실행 가능 |
| Implementation | 5개 app 예제의 child main |
| Verification | Standalone/Supervised 실행; 증거 종류는 [검증 문서](06_VERIFICATION_AND_LIMITATIONS.md) |
| Current Status | Retained |
| Future Revisit Trigger | 새 deployment 모델 필요 시; 현재 결정/구현 약속 아님 |

## D-003 — Supervisor is not data broker

| 항목 | 기록 |
| --- | --- |
| ID | D-003 |
| Date/Stage | 현재 checkpoint; 최초 날짜 미확인 |
| Decision Source | User |
| Context | 실행 관리와 데이터 경로 구분 |
| Decision | 사용자 SHM/UDP 직접 통신 |
| Reason | 중계 책임/병목 추가 방지 |
| Implementation | Bringup + 직접 IPC API |
| Verification | 통신 Integration; 증거 종류는 [검증 문서](06_VERIFICATION_AND_LIMITATIONS.md) |
| Current Status | Retained |
| Future Revisit Trigger | 분산 transport 요구 시; 현재 결정/구현 약속 아님 |

## D-004 — Standalone + Supervised

| 항목 | 기록 |
| --- | --- |
| ID | D-004 |
| Date/Stage | 현재 checkpoint; 최초 날짜 미확인 |
| Decision Source | User |
| Context | 동일 구현을 독립/통합 실행 |
| Decision | FD launch intent와 Runtime 검증 |
| Reason | 중복 executable 정책 방지 |
| Implementation | DetectLaunchExecutionMode/StartSupervision |
| Verification | 5개 app 실행; 증거 종류는 [검증 문서](06_VERIFICATION_AND_LIMITATIONS.md) |
| Current Status | Retained |
| Future Revisit Trigger | launch protocol 변경 시; 현재 결정/구현 약속 아님 |

## D-005 — Runtime lifecycle/error contract

| 항목 | 기록 |
| --- | --- |
| ID | D-005 |
| Date/Stage | 현재 checkpoint; 최초 날짜 미확인 |
| Decision Source | User-approved design + AI-assisted implementation |
| Context | partial Setup 자원과 fatal Loop 오류 |
| Decision | Setup 진입 후 Shutdown 1회; nonzero fatal |
| Reason | 정리 일관성과 오류 보존 |
| Implementation | process_runtime.cpp |
| Verification | runtime_lifecycle_test; 증거 종류는 [검증 문서](06_VERIFICATION_AND_LIMITATIONS.md) |
| Current Status | Implemented |
| Future Revisit Trigger | 새 lifecycle 책임 추가 시; 현재 결정/구현 약속 아님 |

## D-006 — Callback snapshot / Loop processing

| 항목 | 기록 |
| --- | --- |
| ID | D-006 |
| Date/Stage | 현재 checkpoint; 최초 날짜 미확인 |
| Decision Source | User + design refinement |
| Context | worker와 Application state의 동시 접근 |
| Decision | 작은 snapshot/metadata 공유, 긴 작업은 Loop |
| Reason | 경합과 lifetime 책임 명확화 |
| Implementation | pubsub/parameter/action/timer app |
| Verification | 각 app 실제 실행; 증거 종류는 [검증 문서](06_VERIFICATION_AND_LIMITATIONS.md) |
| Current Status | Implemented; API가 강제하지 않음 |
| Future Revisit Trigger | callback 실행 모델 변경 시; 현재 결정/구현 약속 아님 |

## D-007 — Stable TypeDescriptor

| 항목 | 기록 |
| --- | --- |
| ID | D-007 |
| Date/Stage | 현재 checkpoint; 최초 날짜 미확인 |
| Decision Source | User generic Tool requirement → AI/Codex implementation design → user review and validation |
| Context | Application별 Tool 코드 종속 |
| Decision | canonical name/type_id와 flat fields |
| Reason | 일반 field decode/encode |
| Implementation | type_descriptor.hpp/R3 |
| Verification | type_descriptor_test 및 Tool 예제; 증거 종류는 [검증 문서](06_VERIFICATION_AND_LIMITATIONS.md) |
| Current Status | Implemented |
| Future Revisit Trigger | variable/nested schema 확장 시; 현재 결정/구현 약속 아님 |

## D-008 — Runtime/Endpoint/Supervisor introspection

| 항목 | 기록 |
| --- | --- |
| ID | D-008 |
| Date/Stage | 현재 checkpoint; 최초 날짜 미확인 |
| Decision Source | User generic Tool requirement → AI/Codex implementation design → user review and validation |
| Context | generic structure 발견 |
| Decision | 관찰 registry와 instance identity |
| Reason | Tool 변경 없는 Application 발견 |
| Implementation | R1/R2A/R2B public APIs |
| Verification | registry/discovery/KT-8; 증거 종류는 [검증 문서](06_VERIFICATION_AND_LIMITATIONS.md) |
| Current Status | Implemented |
| Future Revisit Trigger | live observation 요구 시; 현재 결정/구현 약속 아님 |

## D-009 — Application-scoped SystemStatus

| 항목 | 기록 |
| --- | --- |
| ID | D-009 |
| Date/Stage | R2B.1 / KT-7 이후 |
| Decision Source | multiple Application 독립 실행 요구 = User; collision 발견 = validation; scoped corrective design = AI/Codex proposal; 적용/유지 = user accepted through implementation and validation |
| Context | 두 번째 Bringup global SHM collision |
| Decision | Supervisor PID/start_ticks scope; Reset scope 유지 |
| Reason | 독립 multiple Applications |
| Implementation | system_status_scope.hpp/Bringup |
| Verification | application_scope_test + KT-8; 증거 종류는 [검증 문서](06_VERIFICATION_AND_LIMITATIONS.md) |
| Current Status | Resolved historical failure |
| Future Revisit Trigger | 외부 observer/namespace 정책 변경 시; 현재 결정/구현 약속 아님 |

## D-010 — Dynamic SHM recreate detection

| 항목 | 기록 |
| --- | --- |
| ID | D-010 |
| Date/Stage | R4.1 / KT-4 이후 |
| Decision Source | KT-4 failure → corrective implementation; 사용자 제공 이력 및 KT-4.1 기록 |
| Context | 같은 PID/type 재생성 object 접근 |
| Decision | 열린 SHM device/inode identity 재검증 |
| Reason | old session을 새 object에 자동 연결하지 않음 |
| Implementation | dynamic_access.cpp/R4.1 |
| Verification | shm_incarnation_test/KT-4.1; 증거 종류는 [검증 문서](06_VERIFICATION_AND_LIMITATIONS.md) |
| Current Status | Implemented; atomic unlink race는 별도 한계 |
| Future Revisit Trigger | namespace transaction 필요 시; 현재 결정/구현 약속 아님 |

## D-011 — Service protocol type validation

| 항목 | 기록 |
| --- | --- |
| ID | D-011 |
| Date/Stage | 현재 checkpoint; 최초 날짜 미확인 |
| Decision Source | Tool dynamic Service requirement → AI-assisted implementation |
| Context | byte 크기만 같은 잘못된 타입 호출 |
| Decision | request/response type/layout 검증 |
| Reason | generic Call의 형식 오류 거부 |
| Implementation | Service protocol 2/R5 |
| Verification | service_introspection_test; 증거 종류는 [검증 문서](06_VERIFICATION_AND_LIMITATIONS.md) |
| Current Status | Implemented |
| Future Revisit Trigger | protocol 호환성 변경 시; 현재 결정/구현 약속 아님 |

## D-012 — Manual Service port assignment

| 항목 | 기록 |
| --- | --- |
| ID | D-012 |
| Date/Stage | 현재 checkpoint; 최초 날짜 미확인 |
| Decision Source | Current implementation → 이후 사용자 확인 및 현 버전 유지 결정; 최초 User 설계로 소급하지 않음 |
| Context | port는 process endpoint |
| Decision | Create(port), 사용자/Application 지정 |
| Reason | 자동 registry/allocator 범위 확대 방지 |
| Implementation | ServiceServer/Action app CLI |
| Verification | bind/restart/Service·Action 예제; 증거 종류는 [검증 문서](06_VERIFICATION_AND_LIMITATIONS.md) |
| Current Status | Retained for current version |
| Future Revisit Trigger | 동시 Application port 관리 요구 시; 현재 결정/구현 약속 아님 |

## D-013 — Timer not introspected

| 항목 | 기록 |
| --- | --- |
| ID | D-013 |
| Date/Stage | 현재 checkpoint; 최초 날짜 미확인 |
| Decision Source | Timer가 process-local feature라는 Current architecture; endpoint 누락 bug가 아님 |
| Context | Timer는 process-local worker |
| Decision | 외부 endpoint를 만들지 않음 |
| Reason | IPC 기능과 구분 |
| Implementation | Timer/registry 부재 |
| Verification | timer_app Runtime-only discovery; 증거 종류는 [검증 문서](06_VERIFICATION_AND_LIMITATIONS.md) |
| Current Status | Intentional |
| Future Revisit Trigger | 명시적 Timer 관찰 요구 시; 현재 결정/구현 약속 아님 |

## D-014 — Action Tool UI deferred

| 항목 | 기록 |
| --- | --- |
| ID | D-014 |
| Date/Stage | 현재 checkpoint; 최초 날짜 미확인 |
| Decision Source | v0.1 Tool scope에서 defer된 기능 (User) |
| Context | 현재 Tool 범위 제한 |
| Decision | 전용 Action model/UI 미구현 |
| Reason | Core Action 검증과 GUI 확장 분리 |
| Implementation | Tool 지원 범위 문서/Action app |
| Verification | console lifecycle, Runtime discovery; 증거 종류는 [검증 문서](06_VERIFICATION_AND_LIMITATIONS.md) |
| Current Status | Deferred |
| Future Revisit Trigger | Action UI 별도 요구 시; 현재 결정/구현 약속 아님 |

## D-015 — Generic Tool Topic Publish omitted

| 항목 | 기록 |
| --- | --- |
| ID | D-015 |
| Date/Stage | 현재 checkpoint; 최초 날짜 미확인 |
| Decision Source | User-provided decision/rationale in this documentation request |
| Context | generic write가 hardware 명령 경로를 직접 건드릴 수 있음 |
| Decision | Tool Topic은 Echo/read 중심 |
| Reason | 명시적인 Application 제어 경로 필요 |
| Implementation | Tool에 generic Publish UI 없음 |
| Verification | 코드/Tool 범위 확인; 위험 자체를 hardware 시험한 것은 아님; 증거 종류는 [검증 문서](06_VERIFICATION_AND_LIMITATIONS.md) |
| Current Status | Omitted; 최초 결정 당시 독립 기록은 확인되지 않음 |
| Future Revisit Trigger | 권한·명령 계약을 포함한 별도 설계 시; 현재 결정/구현 약속 아님 |


Repository branch/file 정리 및 문서 이동 과정에는 사용자가 직접 수행한 수동 Git 작업이 포함되어 있다.
Decision Log는 기능 설계/구현 출처를 추적하며 모든 git operation의 수행 주체를 기록하는 문서는 아니다.

## D-016 — Topic Depth Queue / Snapshot compatibility

| 항목 | 기록 |
| --- | --- |
| ID | D-016 |
| Date/Stage | v5.1 Topic Queue; dev 작업 트리에서 버전 확정 |
| Decision Source | 사용자 Topic Queue 구현 요청 및 문서 반영 요청; AI-assisted implementation |
| Context | 기존 Snapshot API와 복구·pin 보호를 유지하면서 최근 N개 메시지 순차 소비 추가 |
| Decision | 기본 depth=1, depth=N KEEP_LAST, 논리 Entry별 Triple Buffer; Subscriber 독립 Cursor와 NEXT/OLDEST |
| Read contract | ReadNext만 순차 Cursor 이동; ReadLatest/Callback/DynamicTopicReader/Tool Echo는 최신값 유지 |
| Failure contract | 안전한 Write Slot 부족은 -EAGAIN, 실패한 Publish는 Queue/Sequence 불변; 읽기 실패도 Cursor 불변 |
| Sequence / Payload | Commit 성공 시 64-bit Transport Sequence 증가, Overflow 누락 수 제공; 측정 Sequence/Timestamp/Validity는 Application 책임 |
| Compatibility | Topic SHM Format 4, Parameter Format 3 유지; 이전 Topic과 바이너리 비호환, 참여 바이너리 재빌드, 자동 삭제·변환 없음 |
| Retained limits | 단일 Publisher, dead-reader pin 잔존, 명시적 재연결, KEEP_LAST 유실 가능성 |
| Implementation | SharedChannel/Publisher/Subscriber 및 DynamicTopicReader |
| Verification | 구현 완료 보고: 전체 빌드, Queue 10개 영역, 기존 회귀 19개, Tool Backend/Topic Echo PASS; 당시 문서 반영 시 재실행 없음. [이전 보고](06_VERIFICATION_AND_LIMITATIONS.md#topic-queue-verification) |
| Current Status | KCF Framework v5.1로 확정; GitHub dev [82fa3b2](https://github.com/Nyamkani/kss_control_framework/commit/82fa3b274f51d847602b2a5e4936778ebd631257) 반영 확인; tag/Release는 [상태 문서](V5_1_STATUS.md) 참조. [최종 검증](06_VERIFICATION_AND_LIMITATIONS.md#v51-final-verification) |
| Future Revisit Trigger | reader-liveness 회수 또는 전달 보장 요구 변경 시 별도 설계 |
