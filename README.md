# KSS Control Framework (KCF)

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
| Topic | SHM latest-value Publisher/Subscriber |
| Parameter | SHM 현재값 Get/Set, 선택적 watcher |
| Service | localhost UDP request/response, 명시적 port/service_id |
| Timer | process-local periodic callback worker |
| Action | Service와 status Topic 기반 Goal/Feedback/Result/Cancel |
| Introspection | Runtime/Endpoint/Supervisor/type/Service metadata |
| kcf_tools 연동 | Application 탐색, Topic Echo, Parameter 편집, Service Call |

Standalone은 Element executable을 직접 실행합니다. Supervised는 Bringup이 **동일 executable**을
spawn하고 supervision FD를 전달합니다. Supervisor는 사용자 데이터 broker가 아닙니다.

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
- [KCF Framework v5.0 Development Status](docs/V5_0_STATUS.md)

이 문서 세트의 KCF Framework v5.0은 **development status / checkpoint candidate**입니다. 정확한 release commit/tag는
TBD이며, 과거 v4.3 baseline이나 Tool v0.1과 같은 release 번호를 뜻하지 않습니다.
[기존 Readme.md](Readme.md)와 [Changelog.md](Changelog.md)는 역사 기록으로 보존합니다.
현재 구현과 과거 기록의 차이는 [출처·차이 기록](docs/00_PROJECT_SCOPE_AND_DECISIONS.md)에 정리했습니다.
