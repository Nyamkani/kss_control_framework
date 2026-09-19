# KSS Control Framework (KCF) v5.1

Linux 기반 제어 시스템에서 Process lifecycle과 IPC 구조를 얇게 공통화하는 C++17 Framework입니다.

유지보수성을 높이기 위해 기능별 process를 분리하고, 각 Element를 독립적으로 개발·실행·검증합니다.
Process 간 데이터 교환은 명시적인 KCF API로 수행하며 Bringup/Supervisor가 Application lifecycle을 관리합니다.

```text
Application
└─ Bringup / Supervisor
   ├─ Element Process
   │  └─ ProcessRuntime
   │     └─ ProcessElement
   │        ├─ Driver
   │        ├─ Algorithm / FSM
   │        └─ KCF APIs
   └─ ...
```

| 기능 | 현재 역할 |
| --- | --- |
| ProcessRuntime | Setup / Loop / Shutdown, signal, 주기와 오류 처리 |
| Supervisor | spawn/reap, startup barrier, health, 최초 오류 latch, 명시적 Reset |
| Topic | SHM Depth 기반 Bounded Ring Buffer; 기본 depth=1 Snapshot, depth=N KEEP_LAST |
| Parameter | SHM 현재값 Get/Set, 선택적 watcher |
| Service | localhost UDP request/response, 명시적 port/service_id |
| Timer | process-local periodic callback worker |
| Action | Service와 status Topic 기반 Goal/Feedback/Result/Cancel |
| Introspection | Runtime/Endpoint/Supervisor/type/Service metadata |
| kcf_tools 연동 | Application 탐색, Topic Echo, Parameter 편집, Service Call |

Standalone은 Element executable을 직접 실행합니다. Supervised는 Bringup이 **동일 executable**을
spawn하고 supervision FD를 전달합니다. Supervisor는 사용자 데이터 broker가 아닙니다.

## v5.1 — Topic Queue

현재 `dev` 작업 트리의 Topic Queue 확장을 **KCF Framework v5.1**로 확정합니다. CMake project version은 `5.1.0`입니다.
Publisher가 Depth를 지정하고 Subscriber는 SHM Header에서 확인합니다. Subscriber별 독립 Cursor로
`ReadNext`를 순차 소비하며, 기본 `NEXT`는 연결 이후 메시지부터, `OLDEST`는 현재 보관된 가장 오래된 메시지부터 시작합니다.
`ReadLatest`와 기존 Callback은 최신값 방식이며 순차 Cursor를 변경하지 않습니다.
Overflow 누락 수는 `TopicReadInfo::missed`로 확인합니다. Publish의 안전한 Slot 부족 또는 읽기 경합은
`-EAGAIN`이며, 실패한 Publish는 Queue/Sequence를, 실패한 읽기는 Cursor를 전진시키지 않습니다.
Transport Sequence는 Commit 성공 시 증가하는 64-bit 값이고, Payload의 측정 Sequence·Timestamp·Validity는 Application 책임입니다.

Topic SHM은 **Format 4**, Parameter는 **Format 3**입니다. 기존 Topic Format 3과 바이너리 호환되지 않으므로
Publisher/Subscriber와 Tool 등 참여 바이너리를 호환 Core로 재빌드해야 합니다. 이전 SHM은 자동 삭제·변환하지 않습니다.
단일 Publisher, dead-reader pin 잔존, 명시적 재연결 제한은 유지합니다.
[API 계약](docs/03_IPC_AND_FEATURES.md)과 [사용 가이드](docs/05_APPLICATION_DEVELOPMENT_GUIDE.md)를 참조하세요.

v5.1 최종 실행: C++17 전체 빌드, Queue 필수 10개 영역, Framework 회귀 **22/22**
(기존 runner 19개 + 추가 3개), 기존 kcf_tools Backend/Topic Echo **1/1 PASS**.
[최종 검증 기록](docs/06_VERIFICATION_AND_LIMITATIONS.md#v51-final-verification)은
[이전 Queue 보고](docs/06_VERIFICATION_AND_LIMITATIONS.md#topic-queue-verification) 및 v5.0 이력과 구분합니다.

## Quick Start

Linux, CMake 3.16 이상, C/C++ compiler와 POSIX Threads가 필요합니다. Core는 Qt에 의존하지 않습니다.
저장소 루트에서:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j4
./build/examples/pubsub/kcf_pubsub_bringup
```

Publisher/Subscriber 출력 확인 후 Ctrl+C로 종료합니다. 독립 실행과 Tool Echo는
[Pub/Sub 입문 예제](examples/pubsub/README.md)를 따릅니다.

## Application 예제

| 예제 | 학습 내용 |
| --- | --- |
| [pubsub](examples/pubsub/README.md) | callback snapshot, Loop 출력, 동일 executable의 두 실행 모드 |
| [parameter_app](examples/parameter_app/README.md) | Owner/Get/watcher, Tool Apply/Revert |
| [service_app](examples/service_app/README.md) | request/response, Tool Call, port 지정 |
| [timer_app](examples/timer_app/README.md) | 500 ms callback, Stop/join |
| [action_app](examples/action_app/README.md) | Success/Cancel, 완료 후 idle/RUNNING |

## 문서

- [범위·사용자 결정](docs/00_PROJECT_SCOPE_AND_DECISIONS.md)
- [Architecture](docs/01_ARCHITECTURE.md)
- [Runtime / Supervision](docs/02_RUNTIME_AND_SUPERVISION.md)
- [IPC / Features](docs/03_IPC_AND_FEATURES.md)
- [Introspection / Tool](docs/04_INTROSPECTION_AND_TOOL.md)
- [Application 개발 가이드](docs/05_APPLICATION_DEVELOPMENT_GUIDE.md)
- [검증·한계](docs/06_VERIFICATION_AND_LIMITATIONS.md)
- [설계 결정 기록](docs/07_DESIGN_DECISION_LOG.md)
- [KCF Framework v5.1 Status](docs/V5_1_STATUS.md)
- [기존 v5.0 이력](docs/V5_0_STATUS.md)

v5.0 기능 구현·검증의 기존 checkpoint는 `feature/introspection`에 기록되어 있습니다. 현재 `dev` 작업 트리는 Topic Queue를 v5.1로 정리했으며 이번 작업에서 commit/push/tag 생성은 하지 않습니다.
v5.1 버전 확정은 Git release 게시와 별개입니다. 정확한 release commit/tag는 아직 지정하지 않았으며, 과거 v4.3 baseline이나 Tool v0.1과 같은 release 번호를 뜻하지 않습니다.
`README.md`는 현재 v5.1 entry point입니다. [Readme.md](Readme.md)는 historical v4.3 development record이며,
[Changelog.md](Changelog.md)와 함께 역사 기록으로 보존합니다.
현재 구현과 과거 기록의 차이는 [출처·차이 기록](docs/00_PROJECT_SCOPE_AND_DECISIONS.md)에 정리했습니다.
