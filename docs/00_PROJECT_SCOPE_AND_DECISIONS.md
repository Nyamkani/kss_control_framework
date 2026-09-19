# 00. Project Scope and Decisions

현재 안내 기준: KCF Framework v5.1, `dev`. 아래 요구·출처 기록은 2026-09-16 v5.0 checkpoint에서 시작되었으며
현재 버전의 GitHub 상태는 [v5.1 상태](V5_1_STATUS.md)에 정리합니다.
현재 repository code가 구현 상태의 source of truth입니다. 이 문서는 기능 변경이나 release 선언이 아닙니다.

## 근거와 표기

- **[USER REQUIREMENT / DECISION]**: 이번 문서 요청 또는 대화에 명시된 사용자 요구·선택.
- **[IMPLEMENTED]**: 링크된 현재 코드에서 확인한 동작.
- **[IMPLEMENTATION ORIGIN]**: 요구 출처와 구현 주체를 분리한 기록. 확인되지 않은 개별 기여는 추정하지 않습니다.
- **[AUTOMATED VERIFIED]**: 자동 test/regression으로 확인한 결과.
- **[MANUALLY VERIFIED]**: 사용자가 GUI/console/process 실행으로 직접 확인한 결과.
- **[HISTORICAL FAILURE]**: 과거 발견됐으나 현재 수정 완료된 문제.
- 실행 시점·출처·종류는 [검증 문서](06_VERIFICATION_AND_LIMITATIONS.md) 참조.
- **[CURRENT LIMITATION]**: 보장하지 않거나 미구현인 사항.
- **[FUTURE OPTION]**: 미결정 개선 후보이며 현재 API 약속이 아님.

사용자 제공 이력(예: D-009 승인)은 **사용자 제공 출처 기록**으로 표시합니다.
독립된 승인 artifact/commit이 없으면 그것까지 검증되었다고 쓰지 않습니다.

## 프로젝트 목적

[USER REQUIREMENT / DECISION] KCF는 ROS 수준의 범용 middleware 재구현이 아닙니다.
Linux 제어 시스템의 반복 실행 구조를 공통화하며 기능별 process 분리, Element 독립 실행/개발/검증,
Supervisor lifecycle 관리, 명시적 데이터 공유를 목표로 합니다. Linux/POSIX를 과도하게 숨기지 않습니다.

| 책임 | 의미 |
| --- | --- |
| Framework | 공통 실행·통신 기반 |
| Application | 실제 시스템 구성·기능·안전 정책 |
| Bringup/Supervisor | Application 전체 process manager |
| ProcessRuntime | Element process 내부 실행 환경 |
| Element | 하나의 주요 기능 process와 사용자 lifecycle 구현 |
| Component | Driver / Algorithm / Interface 등의 Application 내부 구성 단위 |

Component/Driver/FSM은 책임 구분이며, 현재 generic Component base class나 Driver framework가
구현되어 있다는 뜻은 아닙니다.

Non-goals: ROS 수준 generic middleware, Supervisor data broker, 모든 내부 상태 자동 introspection,
초기 package/plugin ecosystem, distributed remote middleware, 자동 코드 생성 시스템.

## 최상위 결정

| Decision | Source | Reason | Implemented | Verified | Notes |
| --- | --- | --- | --- | --- | --- |
| 기능별 process | User | 독립 개발·격리 | Process/Runtime | spawn/lifecycle 회귀 | OS crash 격리와 기능 안전은 별개 |
| 동일 executable 두 실행 모드 | User | 독립 검증과 통합 재사용 | ExecutionMode + FD | 5개 app 예제 | mode는 launch intent |
| Supervisor는 broker 아님 | User | 제어와 데이터 경로 분리 | 직접 SHM/UDP | 통신 통합 | SystemStatus는 제어 metadata |
| 명시적 오류/Reset | User-approved design | 최초 실패 원인 보존 | ERROR latch/RequestReset | Reset 회귀 | 자동 restart 없음 |
| 새 app에도 Tool 수정 없음 | User | 타입별 GUI 종속 제거 | R1–R5 | 예제 UI 및 KT-8 | stable descriptor 필요 |
| 수동 UDP port | Current implementation; Action 요청에서 유지 명시 | endpoint 소유를 Application이 결정 | Create(port) | Service/Action 예제 | 최초 사용자 설계였다고 소급 귀속하지 않음 |

[IMPLEMENTATION ORIGIN] 최근 5개 app 예제는 사용자 지정 구조·계약을 기준으로 Codex가 구현했습니다.
port별 Action status 이름, 예제 CMake target 충돌 회피, bounded startup 재시도 등은 구현 세부 선택입니다.
Core 전체의 line별 작성자를 이 문서만으로 판정하지 않습니다.

## 기존 기록과 현재의 차이

| 기록 | 현재 해석 |
| --- | --- |
| 통합 전 README의 v4.3, discovery 제외 설명 | 과거 baseline 기록. 원문은 Git history, 보존 이력은 [Changelog](../Changelog.md)와 [검증 문서](06_VERIFICATION_AND_LIMITATIONS.md). 현재 R1–R5 API 지원 |
| [R1 README](../examples/introspection/README.md)의 R2 이후 Deferred | R1 단계의 미래 계획. 현재 R4는 Topic/Parameter dynamic access, R5는 Service dynamic call |
| R1 README의 `R2B1.md` 링크 | [R2B.1 문서](../examples/introspection/R2B1.md)가 현재 존재함. 초기 Deferred 표시는 historical stage 기록이며 R2A/R2B/R2B.1/R3/R4/R4.1/R5는 후속 구현됨 |
| 과거 KCF_ARCHITECTURE_PLAN / KCF_CODEX_INSTRUCTION | 과거 설계 지시 문서의 보관/이동 이력과 현재 구현 상태를 구분하며, 현재 repository code를 우선함 |
| 과거 v0.1 / v4.x / v5.0와 현재 v5.1 | 과거 단계 번호와 [v5.0 checkpoint](V5_0_STATUS.md)는 보존. 현재 Topic Queue와 호환성은 [v5.1](V5_1_STATUS.md) 기준 |
| KT-7 두 번째 Supervisor 충돌 | 과거 failure. R2B.1 현재 구현 및 후속 검증으로 해결; 현재 일반 제한으로 나열하지 않음 |

루트 README를 하나로 통합했습니다. 구 Readme.md는 삭제하고 유효한 실행법·검증 요약은 개발 가이드·검증 문서에 보존했습니다.
새 historical README 사본은 만들지 않으며 과거 원문은 Git history에서 확인합니다.
Mecanum Application은 현재 Framework repository에 포함되지 않습니다. 공개 docs와 달리 `docs/instructions/`는 Git ignore된 로컬 지침입니다.

[IMPLEMENTATION ORIGIN] v5.0 Framework의 introspection/dynamic access 확장은 사용자의 generic Tool 요구를
기준으로 AI-assisted implementation(Codex)으로 구현되었다. 문서 정리 및 branch/file 이동은 사용자가
직접 수행한 부분도 있으며, repository history만으로 작업 주체를 임의 추정하지 않는다.
