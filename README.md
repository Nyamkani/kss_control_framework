# KSS Control Framework (KCF) v5.2

KCF는 Linux 제어 시스템의 Process lifecycle, 실행·통신 구조를 공통화하는 C++17 Framework입니다.
로봇·산업 장비·센서·액추에이터·SBC 기반 시스템에서 기능별 process를 독립적으로 개발·실행·검증하고,
반복되는 Linux/POSIX 코드를 줄이는 것이 목적입니다. ROS를 재구현하거나 OS 동작을 과도하게 숨기지 않습니다.

## Architecture 및 책임 구조

```text
Application — 시스템 구성 / 제어 / FSM / 안전 정책
└─ Bringup / Supervisor — Application process 관리
   └─ Process — child 생성·종료·회수
      └─ ProcessRuntime — lifecycle / 주기 / signal / supervision
         └─ ProcessElement — Setup / Loop / Shutdown
            └─ Application Component — Driver / Algorithm / Interface
```

Framework는 공통 실행·통신을, Application은 실제 장치·알고리즘·판단을 담당합니다.
제어 Backend는 UI 없이 동작하고 Frontend는 표시·입력을 담당합니다. Supervisor는 사용자 Payload를 중계하거나
Element 내부 기능을 직접 호출하지 않습니다. **Mecanum Application은 현재 Framework repository에 포함되지 않습니다.**

Standalone은 Element executable을 직접 실행하고, Supervised는 Bringup이 **동일 executable**을 생성합니다.
Setup 시작 후에는 실패해도 Shutdown을 한 번 수행합니다. Loop의 nonzero 반환은 fatal 오류이며,
일시적인 오류 복구는 Element 책임입니다. Supervisor는 최초 오류를 유지하고 자동 재시작하지 않습니다.
명시적 Reset은 전체 child generation을 교체하며 모두 RUNNING일 때만 오류를 해제합니다.
SystemStatus는 **Application instance(PID + start_ticks)별 scope**를 사용하고 Reset 동안 같은 scope를 유지합니다.
[Architecture](docs/01_ARCHITECTURE.md), [Runtime·오류·Reset](docs/02_RUNTIME_AND_SUPERVISION.md)에 세부 계약이 있습니다.

## 현재 구현 기능

| 기능 | 역할 |
| --- | --- |
| Process / Runtime / Supervisor | lifecycle, spawn/reap, health, 최초 오류 latch, 명시적 Reset |
| Topic | Depth 기반 Bounded Queue; 기본 depth=1 Snapshot, depth=N KEEP_LAST |
| Parameter | SHM 현재값 Get/Set, 선택적 watcher |
| Service | localhost UDP request/response, 명시적 port/service_id |
| Timer | process-local periodic callback worker |
| Action | Service + status Topic 기반 Goal/Feedback/Result/Cancel |
| Network | Discovery v2, compatibility, Control TCP v1, Remote Parameter/Service/Action, UDP Remote Topic; 명시적 allowlist와 5종 Target |
| Introspection | Runtime/Endpoint/Supervisor/type/Service/Action metadata와 dynamic access |

Network Action은 기존 Local Action을 호출하며, Application에 별도 Network 등록을 요구하지 않습니다.
`metadata.remote_actions`로 노출을 허용하고 `ControlNode::StartAction` / `CancelAction`을 사용합니다.
Feedback은 bounded latest 방식이며 Cancel 수락과 terminal 취소를 구분합니다.
**연결 소실은 Local Action을 취소하지 않습니다.** Goal/Cancel timeout에는 적용 여부가 불확실할 수 있고,
자동 retry 및 기존 Goal 재연결은 없습니다. [Network 계약과 사용법](docs/08_NETWORK_ARCHITECTURE.md#remote-action--공통-control과-local-action-연결)을 확인하세요.

Remote Topic은 `remote_topics` allowlist와 실제 compatible Subscriber를 확인한 뒤 UDP unicast로 전달합니다.
Control과 분리된 NORMAL Data Plane이며 best-effort latest-data, 기본 최대 50 Hz, 재전송 없음입니다.
수신 Gateway의 native TypeDescriptor catalog로 빈 Local proxy를 먼저 준비한 뒤 기존 Subscriber를 생성합니다.
Local Topic Format 4와 API는 유지하며 큰 payload는 명시적으로 거부합니다.
Gateway 재시작 시 ownership sidecar로 증명된 죽은 Network proxy만 자동 복구합니다.
일반 Local Publisher·unknown SHM은 삭제하지 않으며, 이전 mapping을 가진 Subscriber는 명시적으로 재연결합니다.
[설정·bootstrap·크기 상한·proxy 수명](docs/08_NETWORK_ARCHITECTURE.md#remote-topic-data-plane)을 확인하세요.

격리된 두 가상 Host의 실제 veth/bridge 통합 시험은
`./tests/network_virtual/run_virtual_network_test.sh`로 실행합니다.
[Virtual LAN 구성·권한·장애 주입·결과](docs/09_VIRTUAL_LAN_TEST.md)를 참고하세요. 일반 빌드와 별도의 opt-in 시험입니다.

## Quick Start

Linux, CMake 3.16 이상, C/C++ compiler(C++17), POSIX Threads가 필요합니다. Core는 Qt에 의존하지 않습니다.
저장소 루트에서:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j4
./build/examples/pubsub/kcf_pubsub_bringup
```

Publisher/Subscriber 출력 확인 후 Ctrl+C로 종료합니다. 가장 작은 Standalone 실행은
`./build/examples/dummy/kcf_dummy`입니다. 상세 실행법은 [Pub/Sub 예제](examples/pubsub/README.md)를 참조하세요.

## Topic Queue 간단 사용법 (v5.1부터, v5.2 유지)

아래는 Publisher/Subscriber 각각의 설정과 읽기 API 요약입니다. `Message`는 Application Payload 타입이며,
실제 코드에서는 각 호출의 반환값과 자원 정리를 처리합니다.

```cpp
kcf::Publisher<Message> publisher;
publisher.Create("/example/data", 8); // 생략하면 depth=1

kcf::Subscriber<Message> subscriber;
subscriber.Create("/example/data");  // NEXT: 연결 이후 새 메시지부터
// 대신 OLDEST로 시작하려면:
// subscriber.Create("/example/data", kcf::TopicStartPosition::OLDEST);

Message value{};
kcf::TopicReadInfo info{};
int result = subscriber.ReadNext(value, info); // 성공 시 sequence / missed 확인
result = subscriber.ReadLatest(value, info);  // 최신값, 순차 Cursor 불변
```

Subscriber별 독립 Cursor를 사용하며 `OLDEST`는 현재 보관된 가장 오래된 메시지부터 읽습니다.
Overflow는 KEEP_LAST이고 누락 수는 `info.missed`로 전달합니다. 기존 Callback과 Tool Echo는 최신값 방식이며
ReadNext의 Cursor에 영향을 주지 않습니다. 안전한 Write Slot 부족, 읽기 경합 또는 읽을 값 부재는 `-EAGAIN`입니다.
실패한 Publish는 Queue/Sequence를, 실패한 읽기는 Cursor를 전진시키지 않습니다.
64-bit Transport Sequence는 Commit 성공 시 증가합니다. **Payload의 측정 Sequence·Timestamp·Validity는 Application 책임**이며
KCF는 `valid=false`도 변경 없이 전달합니다. [전체 API 계약](docs/03_IPC_AND_FEATURES.md#topic)을 참고하세요.

## Application 개발 및 Examples

Setup은 초기화, Loop는 제어·FSM·I/O, Shutdown은 부분 초기화까지 고려한 정리를 담당합니다.
Callback은 별도 worker에서 Loop와 동시에 실행될 수 있습니다. 공유 상태를 짧게 동기화하고 긴 연산·장치 I/O는
Loop에서 수행합니다. [개발 가이드](docs/05_APPLICATION_DEVELOPMENT_GUIDE.md)에 실행 모드·동시성·종료 계약을 정리했습니다.

| 예제 | 사용법 / 학습 내용 |
| --- | --- |
| [pubsub](examples/pubsub/README.md) | Standalone/Supervised, Callback snapshot, Loop 출력 |
| [parameter_app](examples/parameter_app/README.md) | Owner/Get/watcher, Tool Apply/Revert |
| [service_app](examples/service_app/README.md) | request/response, Tool Call, port 지정 |
| [timer_app](examples/timer_app/README.md) | 500 ms callback, Stop/join |
| [action_app](examples/action_app/README.md) | Success/Cancel, 완료 후 idle/RUNNING |

기존 low-level 예제의 실행 명령은 [개발 가이드](docs/05_APPLICATION_DEVELOPMENT_GUIDE.md#low-level-examples)에 있습니다.
Application의 Driver와 안전 정책은 이 예제들이 대신 제공하지 않습니다.

## kcf_tools 연동

[kcf_tools](https://github.com/Nyamkani/kcf_tools)는 별도 repository입니다. Application/Element 탐색,
Topic Echo, Parameter 편집, Service Call을 지원합니다. Dynamic access에는 명시적인 TypeDescriptor가 필요합니다.
Topology/state는 수동 Refresh, Topic Echo는 polling입니다. Action UI, Timer endpoint, 일반 Topic Publish,
사용자용 Reset/Launch manager는 제공 기능이 아닙니다. [Local 지원 범위](docs/04_INTROSPECTION_AND_TOOL.md)를 참조하세요.

별도 저장소의 **kcf_tools v0.12**는 GitHub main의
[`f4fb447`](https://github.com/Nyamkani/kcf_tools/commit/f4fb4471fc0b03b24663eb20eede8d8491fd87f6)에 반영되어 있습니다. Network 관측에서는 공개 PeerDiscovery API로 Remote Host와
Application·Element·T/P/S/A metadata, ONLINE/LOST, version·compatibility·capability를 표시합니다.
Host 계층 Graph와 필터·읽기 전용 Remote Detail을 제공하며, 수동 Refresh는 background worker의 snapshot을 사용합니다.
Remote Action metadata 표시는 Goal/Cancel 조작 UI와 다릅니다. Tool은 Gateway나 payload broker가 아닙니다.

**외부 Gateway의 active route/statistics 및 proxy 판별은 아직 미지원**입니다.
`ControlNode::GetTopicDiagnostics()`는 해당 객체 내부 조회이며 타 process의 공개 관측 API가 없습니다.
따라서 이름/type이 같다는 이유로 cross-host edge를 그리지 않습니다. Remote Set/Call/Goal/Cancel UI도 없습니다.
Tool KCF+Qt 18/18, no-KCF 6/6 PASS는 별도 저장소의 기존 검증 결과이며, Tool+Virtual LAN과 실제 물리 LAN은 미검증입니다.

## 호환성과 주요 제한 사항

- **Topic SHM Format 4 / Parameter Format 3**. 이전 Topic Format 3과 바이너리 비호환이므로 Publisher/Subscriber 및 Tool을 호환 Core로 재빌드해야 합니다. 이전 SHM은 자동 삭제·변환하지 않습니다.
- Local IPC는 같은 Linux 호스트와 호환 Payload ABI가 필요합니다. Payload는 fixed-size trivially-copyable이며 pointer/heap ownership을 포함하지 않습니다. Network는 별도 type/schema/encoding 검증과 명시적 노출 설정을 사용합니다.
- Remote Topic은 유실 없는 Queue 복제가 아닌 best-effort 최신값 전달입니다. 자동 retry·Action re-attach·범용 schema 교환·인증/암호화는 미지원이며 실제 물리 LAN/이기종 장치 검증은 남아 있습니다.
- Topic은 단일 Publisher/쓰기 thread입니다. Dead-reader pin을 자동 회수하지 않으며 Slot 부족은 `-EAGAIN`입니다. KEEP_LAST는 유실 없는 전달 보장이 아닙니다.
- SHM 재생성 후 명시적 재연결이 필요합니다. DynamicTopicReader는 교체를 `-ESTALE`로 알립니다.
- 무한 block된 Setup/Loop의 cleanup, hard real-time, Device watchdog/hardware safety는 보장하지 않습니다. Software SAFE는 장치 안전 기능을 대체하지 않습니다.
- Service timeout은 handler 미실행 보장이 아니며 callback이 반환하지 않으면 Stop/join이 지연될 수 있습니다.

검증 결과와 전체 한계는 [검증 문서](docs/06_VERIFICATION_AND_LIMITATIONS.md)에 기록합니다.

## 상세 문서

- [프로젝트 범위·설계 결정](docs/00_PROJECT_SCOPE_AND_DECISIONS.md)
- [Architecture](docs/01_ARCHITECTURE.md)
- [Runtime / Supervision](docs/02_RUNTIME_AND_SUPERVISION.md)
- [IPC / Features](docs/03_IPC_AND_FEATURES.md)
- [Introspection / Tool](docs/04_INTROSPECTION_AND_TOOL.md)
- [Application 개발 가이드](docs/05_APPLICATION_DEVELOPMENT_GUIDE.md)
- [검증·한계](docs/06_VERIFICATION_AND_LIMITATIONS.md)
- [설계 결정 기록](docs/07_DESIGN_DECISION_LOG.md)
- [Network IPC 공통 계약](docs/08_NETWORK_ARCHITECTURE.md) — Discovery·compatibility·Control·Remote IPC·proxy recovery
- [Virtual 2-Host LAN 검증](docs/09_VIRTUAL_LAN_TEST.md) — 격리된 통합 시험과 장애·재시작 검증

공개 문서는 `docs/`에 포함됩니다. `docs/instructions/`만 Git ignore된 로컬 개발 지침 디렉터리입니다.

## Version / Changelog

현재 버전은 **Framework v5.2 (5.2.0) — Network IPC 확장**입니다.
GitHub dev의 [`0a3f2a6`](https://github.com/Nyamkani/kss_control_framework/commit/0a3f2a630cec209793f9ac6e99107752e9e01283)에 반영되었으며,
이전 v5.1 기준 commit은 [`8360b6e`](https://github.com/Nyamkani/kss_control_framework/commit/8360b6e053472ba20acdfc62b7489e33122997ec)입니다.
v5.1 Topic Queue 위에 Discovery/compatibility, Remote Parameter·Service·Action·Topic,
Virtual LAN 검증과 Network proxy recovery를 포함합니다.
별도 `kcf_tools`의 read-only 관측 연동과 미완료 범위도 [Changelog](Changelog.md#v52--network-ipc--remote-discoverycontroltopic)에 구분하여 기록합니다.

현재 checkout에서는 후속 정합성 보정으로 **CMake project version과 Network `FrameworkVersion` 기본값을 `5.2.0`으로 일치**시켰습니다.
`NETWORK_PROTOCOL`은 Framework 버전 차이를 허용하고 Network major 및 capability 조건을 따릅니다.
`EXACT_FRAMEWORK_VERSION`은 비교 대상 Framework 버전까지 정확히 일치해야 하므로 기본 profile은 `5.2.0`을 요구합니다.
NetworkProtocolVersion 1.0, Discovery v2, Control TCP v1, Topic wire v1 및 기존 storage format은 변경하지 않았습니다.
과거 구현 검증과 이번 보정의 실행 결과는 [검증 기록](docs/06_VERIFICATION_AND_LIMITATIONS.md)에서 구분합니다.
Git tag/GitHub Release 생성 여부는 이번에 확인하지 않았으므로 공개 commit과 동일시하지 않습니다.

[v5.1 상태](docs/V5_1_STATUS.md)와 [v5.0 이력](docs/V5_0_STATUS.md)은 당시 구현·검증 기록으로 보존합니다.
v5.1의 GitHub 반영 및 2026-09-19 당시 tag/Release 확인 결과는 해당 이력 문서에 남깁니다.
Framework 버전과 별도 Tool 버전은 독립적입니다. 통합 전 README 원문은 Git history에서 확인할 수 있습니다.
