# KCF Codex Development Instruction

## 1. Architecture First

작업을 시작하기 전에 항상
docs/KCF_ARCHITECTURE_PLAN.md의 관련 부분을 기준으로 한다.

Architecture Plan과 충돌하는 구조를 임의로 만들지 않는다.


## 2. Limited Scope

사용자가 지정한 작업 범위만 수정한다.

관련 없는 파일을 탐색하거나 수정하지 않는다.

프로젝트 전체 리팩터링을 임의로 수행하지 않는다.


## 3. No Unrequested Abstraction

요청하지 않은 abstraction, manager, factory, plugin system,
middleware layer를 임의로 추가하지 않는다.

KCF는 ROS를 다시 만드는 프로젝트가 아니다.


## 4. Keep OS Structure Visible

Linux Process, Shared Memory, pthread, socket 등의 실제 구조를
지나치게 숨기지 않는다.

Wrapper는 반복 코드를 줄이기 위한 얇은 계층이어야 한다.


## 5. Clear Responsibility

다음 책임을 섞지 않는다.

Bringup
= Child Process lifecycle

ProcessRuntime
= Process 내부 실행 lifecycle

Element
= 기능 단위 Application logic

Component
= Driver / Algorithm / Interface


## 6. Small Incremental Development

한 번의 요청에서 필요한 기능만 구현한다.

미래 기능을 예상하여 대량의 코드를 미리 작성하지 않는다.


## 7. Build Verification

변경 후 필요한 target만 build한다.

관련 없는 전체 테스트나 전체 프로젝트 분석을 수행하지 않는다.


## 8. Report Format

작업 완료 후 다음만 짧게 보고한다.

- 생성/수정한 파일
- 구현한 내용
- 실행한 build/test
- 발견된 문제 또는 남은 작업


## 9. Current Phase

현재 개발 단계는 Phase 1이다.

Phase 1의 대상은 Process Execution Framework이다.

현재 구현하지 않는 항목:

- Topic
- Service
- Action communication
- Parameter
- SharedChannel
- Device Driver
- Mecanum application functions

Architecture Plan에 명시된 Phase 1 순서를 기준으로
단계적으로 구현한다.
