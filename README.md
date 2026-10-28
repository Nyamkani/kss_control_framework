# KSS Control Framework (KCF) v5.6.0 — 개발자 사용 매뉴얼

KCF는 Linux C++17 Application의 프로세스 실행·주기·감시와 IPC를 제공합니다.
이 문서는 저장소 루트에서 예제를 실행하고, 같은 구조로 ProcessElement와 Bringup을 작성하는 순서로 읽습니다.
장치 Driver, 알고리즘, FSM, 데이터 유효성 판단과 안전 정책은 Application이 구현합니다.

## 버전과 사용 범위

현재 로컬 `dev`의 버전은 **Framework v5.6.0**이며, Supervisor S1~S4와 NET56 개발 구현을 포함합니다.
NET56 개발 완료 범위는 v5.6.0 정식 릴리스 완료를 의미하지 않습니다.
루트 [CMakeLists.txt](CMakeLists.txt)의 project version과
[FrameworkVersion](kcf/include/kcf/network/compatibility.hpp)의 기본값은 모두 **5.6.0**입니다.
`NETWORK_PROTOCOL` 정책은 Network major와 capability를 검사하고,
`EXACT_FRAMEWORK_VERSION`은 Framework 버전까지 일치해야 합니다.
NetworkProtocol 1.0 / Discovery v2 / Control TCP v1 / Topic wire v1은 별도 버전입니다.

v5.5 이하 변경·커밋 이력은 [Changelog](Changelog.md), 당시 검증·Known Issue는
[검증 및 제한](docs/06_VERIFICATION_AND_LIMITATIONS.md)을 참조합니다.
Framework와 별도 KCF Tools의 버전은 독립적입니다. 과거 Tool 연동 PASS를 v5.6 재검증으로 해석하지 않습니다.
이 README 정리는 새 tag/Release나 새 회귀·Benchmark 결과를 선언하지 않습니다.

## 1. 개발 환경과 빌드

Linux, CMake 3.16 이상, C/C++ compiler(C++17), POSIX Threads가 필요합니다.
Core/Bringup 빌드에는 Qt가 필요하지 않습니다. Ubuntu/Debian의 기본 패키지 설치 예:

```sh
sudo apt update
sudo apt install build-essential cmake git
```

저장소 루트에서 Debug와 Release는 서로 다른 디렉터리에 빌드합니다.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j4
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release -j4
```

실행 파일은 `build/examples/<예제>/<target>`에 생성됩니다. Release는 앞 경로를
`build-release`로 바꿉니다. 특정 Target만 빌드하려면:

```sh
cmake --build build --target kcf_dummy -j4
cmake --build build --target kcf_pubsub_bringup -j4
```

`kcf_pubsub_bringup` target은 두 child executable도 함께 빌드합니다.
현재 저장소는 명시적 `add_subdirectory` 구성이며 설치된 SDK의 `find_package(KCF)` workflow를 가정하지 않습니다.
새 Application의 CMake 구성은 [실제 Pub/Sub CMake](examples/pubsub/CMakeLists.txt)를 시작점으로 사용합니다.

### 첫 실행과 정상 종료

```sh
./build/examples/dummy/kcf_dummy
```

Setup → 약 2 Hz Loop 출력을 확인하고 Ctrl+C로 종료하면 Shutdown을 수행합니다.
다음은 같은 Pub/Sub executable을 독립 실행하는 방법입니다. 터미널을 나눠 Publisher부터 실행합니다.

```sh
# 터미널 1
./build/examples/pubsub/kcf_pubsub_publisher
# 터미널 2
./build/examples/pubsub/kcf_pubsub_subscriber
```

약 1초마다 같은 sample의 sequence/count/value가 출력됩니다.
Subscriber, Publisher 순으로 각각 Ctrl+C로 종료한 **뒤** Supervised 모드를 실행합니다.
같은 `/example/chatter`에 Publisher 둘을 동시에 실행하지 않습니다.

```sh
./build/examples/pubsub/kcf_pubsub_bringup
```

Supervisor 터미널에서 Ctrl+C 또는 해당 프로세스에 SIGTERM을 보내면 관리 child를 종료·회수합니다.
같은 디렉터리의 세 binary를 함께 유지합니다. 상세 출력·동시성은 [Pub/Sub README](examples/pubsub/README.md)를 참고합니다.

## 2. Framework와 Application 구조

```text
Application — 구성 / Driver / Algorithm / FSM / 안전 정책
└─ Bringup / Supervisor — Managed 프로세스 등록·감시·Reset·종료
   └─ Process — child 생성·exec·회수
      └─ ProcessRuntime — lifecycle / Loop 주기 / signal / supervision
         └─ ProcessElement — Setup / Loop / Shutdown
```

KCF Core는 실행·통신 기반이며 Bringup은 그 위의 Application 프로세스 관리자입니다.
Supervisor는 Topic Payload를 중계하거나 Element의 장치 함수를 직접 호출하지 않습니다.
Frontend 없이도 제어 Backend가 동작하도록 Application을 구성합니다.

| 경로 | 용도 |
| --- | --- |
| `kcf/include/kcf`, `kcf/src` | Core 공개 헤더와 구현, `kcf_core` target |
| `bringup/include`, `bringup/src` | ElementSpec, Bringup/Supervisor |
| `examples/dummy`, `examples/pubsub` | 최소 lifecycle과 첫 Application |
| `examples/*_app`, `examples/application_composition` | 기능별 Application과 Managed/External 조합 |
| `examples/network`, `tests/network_virtual` | Network 예제·격리 LAN 검증 |
| `docs` | 상세 계약·개발 가이드·검증 이력 |

Mecanum Application은 이 저장소에 포함되지 않습니다.
[Architecture](docs/01_ARCHITECTURE.md), [Application 개발 가이드](docs/05_APPLICATION_DEVELOPMENT_GUIDE.md)에 책임 경계를 정리했습니다.

## 3. ProcessElement 작성

다음은 [Dummy 구현](examples/dummy/src/dummy_element.cpp)과 같은 최소 lifecycle입니다.
`main.cpp`에 작성할 수 있는 완전한 C++ 예이며 `kcf_core`에 링크합니다.

```cpp
#include "kcf/process/process_element.hpp"
#include "kcf/process/process_runtime.hpp"
#include <iostream>

class MyElement final : public kcf::ProcessElement {
public:
    int Setup() override {
        std::cout << "Setup\n";
        return 0;
    }
    int Loop() override {
        // 실제 장치 처리/FSM을 한 cycle만 수행합니다.
        return 0;
    }
    void Shutdown() override {
        std::cout << "Shutdown\n";
    }
};

int main() {
    MyElement element;
    kcf::ProcessRuntime runtime;
    runtime.SetLoopFrequency(100.0);
    return runtime.Run(element);
}
```

- `int Setup()`: 자원 생성·초기화. 성공은 0, 실패는 nonzero(음수 errno 권장).
- `int Loop()`: 한 cycle 성공은 0. nonzero는 fatal이며 lifecycle을 끝냅니다.
  운전을 계속할 수 있는 일시적 오류만 Element가 직접 처리하고 0을 반환합니다.
- `void Shutdown()`: Setup에 진입했다면 Setup 실패에도 정확히 한 번 호출됩니다.
  첫 자원 성공 후 두 번째 초기화가 실패할 수 있으므로 소유 여부를 기록하고 획득한 자원만 해제합니다.
  Runtime 자체 초기화/메모리 잠금에서 Setup 전에 실패했다면 Element Shutdown도 호출되지 않습니다.
- `ProcessRuntime::Run(element)`: lifecycle을 실행하며 종료/오류 결과를 반환합니다.
  Unix exit status는 음수 errno 전체를 보존하지 않으므로 Runtime 오류·로그와 함께 확인합니다.

Callback은 Loop와 별도 worker에서 실행될 수 있습니다. 공유 state를 짧게 동기화하고,
Shutdown에서 worker Stop/join 후 캡처된 state를 정리합니다. 무한 block된 Setup/Loop는
정상 정리를 진행할 수 없습니다. [Runtime 계약](docs/02_RUNTIME_AND_SUPERVISION.md)을 참고합니다.

## 4. Standalone과 Supervisor 구성

동일 Element 실행 파일을 직접 실행하면 Standalone, Bringup이 spawn하면 Supervised입니다.
Application에서 모드를 표시할 필요가 있으면 `DetectLaunchExecutionMode()`를 Run 전에 사용합니다.
별도 Supervised 전용 executable을 만들지 않습니다.

다음 Bringup main은 위에서 빌드한 Pub/Sub binary를 사용합니다. 저장소 루트에서 실행하는 예로,
실제 예제는 [bringup_main.cpp](examples/pubsub/src/bringup_main.cpp)처럼 자기 binary 위치로 sibling 경로를 찾습니다.

```cpp
#include "bringup/bringup.hpp"
#include <iostream>
#include <vector>

int main() {
    std::vector<bringup::ElementSpec> elements{
        {"publisher", "./build/examples/pubsub/kcf_pubsub_publisher", {}},
        {"subscriber", "./build/examples/pubsub/kcf_pubsub_subscriber", {}}
    };
    // 기본 Profile(INHERIT/0/빈 affinity/lock_memory=false) 대신 명시적 OTHER 예
    kcf::ExecutionProfile profile;
    profile.policy = kcf::SchedulingPolicy::OTHER;
    elements[0].execution_profile = profile;

    bringup::RegistrationError error;
    int code = bringup::Bringup::ValidateElements(elements, &error);
    if (code != 0) {
        std::cerr << error.element_name << ": " << error.field
                  << " / " << error.reason << '\n';
        return code;
    }
    bringup::Bringup supervisor;
    code = supervisor.Setup("pubsub_example", elements);
    if (code != 0) return code;
    return supervisor.Run();
}
```

Bringup 구현과 `kcf_core` 링크는 [예제 CMake](examples/pubsub/CMakeLists.txt)를 따릅니다.
`ElementSpec`은 name, executable, arguments, startup/health/shutdown timeout 및 execution_profile을 가집니다.
Timeout 기본값은 각각 5000/2000/1500 ms입니다.
`ValidateElements()`는 fork·SHM 생성 없이 빈 목록/이름/경로, 0 Timeout, 중복 이름을 검사하며
최초 오류를 `-EINVAL`과 `RegistrationError`로 반환합니다. `element_index`는 optional 0-based index입니다.
실행 파일 존재·RT 권한·Element Setup 성공까지 검증하는 함수는 아닙니다.

**`Bringup::Setup()==0`은 Supervisor가 Run 가능한 상태라는 뜻입니다.**
자식 실행·초기화 실패 시 ERROR 상태로도 0을 반환할 수 있습니다.
Application RUNNING은 모든 Managed Runtime의 시작 barrier가 완료된 상태입니다.
Supervisor 자체 준비 실패는 nonzero, 초기화 중 명시적 종료는 정리 후 `-ECANCELED`입니다.
[등록 검증 계약](docs/05_APPLICATION_DEVELOPMENT_GUIDE.md#element-registration-validation)을 참조합니다.

### 상태·오류·사용자 요청

| 상태 | 의미 |
| --- | --- |
| INITIALIZING | 등록된 child 실행과 시작 barrier 대기 |
| RUNNING | 모든 Managed Runtime의 시작 완료, 감시 중 |
| ERROR | 장애를 latch하고 명시적 사용자 요청 대기; healthy child는 계속 실행 가능 |
| RESETTING | 전체 Managed generation 종료·회수 후 재생성 |
| SHUTTING_DOWN | 종료 요청 처리와 child 회수 |
| STOPPED | 회수 완료; 이후 SystemStatus SHM 정리 |

`GetApplicationState()`와 `GetApplicationError()`는 현재 상태/최초 Current Error를,
`GetErrorHistory()`는 최근 64건의 장애 이력을 제공합니다. 조회는 lifecycle과 같은 제어 thread에서 사용합니다.
SystemStatus는 Supervisor PID + start_ticks scope이며 Reset 동안 scope를 유지합니다.
SHM 제거 이후 STOPPED를 영구 관측할 수 있다고 가정하지 않습니다.

- **자동 Reset/Restart는 없습니다.** `RequestReset()`은 ERROR에서 명시적으로 요청합니다.
  성공하면 Current Error만 해제하고 History는 유지합니다. 실패하면 ERROR로 돌아가 다음 요청을 기다립니다.
- `RequestShutdown()`은 초기화·운전·오류·Reset 중 정상 종료를 요청하며 Reset보다 우선합니다.
  정상 종료 자체를 새 장애로 기록하지 않지만 과거 오류 때문에 Run은 nonzero일 수 있습니다.
- 두 Request API는 thread-safe 요청이며 signal-handler API가 아닙니다.
  사용자 입력을 연결하는 것은 Application 책임입니다. 일반 예제에 Reset CLI나 Tool 버튼이 있다고 가정하지 않습니다.

Managed는 Supervisor가 실행·종료·회수·Reset하는 프로세스입니다. External/Standalone은 독립 실행되며
Supervisor가 PID를 종료하거나 장애를 자동 ERROR로 전파하지 않습니다. 외부 입력의 timeout·안전 판단은 Application 책임입니다.
Managed 종료로 External의 IPC가 끊길 수 있지만 프로세스 소유권은 별개입니다. Attached API는 미구현입니다.
[Composition 실행 예제](examples/application_composition/README.md)와
[상태 계약](docs/02_RUNTIME_AND_SUPERVISION.md#supervisor-states-and-reset)을 참고합니다.

## 5. IPC 선택과 최소 사용 흐름

Local Payload는 pointer/heap ownership이 없는 fixed-size trivially-copyable 타입이며 같은 호스트의 호환 ABI가 필요합니다.
Tool dynamic access에는 stable TypeDescriptor가 추가로 필요합니다. 각 호출의 실패를 처리하고 소유자만 Unlink합니다.
아래는 호출 순서 안내이며 전체 타입·오류 처리·정리 코드는 연결된 실제 예제를 사용합니다.

| 기능 | 최소 사용 순서 | 실제 예제 |
| --- | --- | --- |
| Topic | Publisher `Create(name, depth)` → `Publish(value)`; Subscriber `Create(name)` → `ReadLatest(value, info)` 또는 `ReadNext(value, info)`; 종료 시 Close, Owner Unlink | [Pub/Sub](examples/pubsub/README.md), [Publisher 소스](examples/pubsub/src/publisher_main.cpp) |
| Parameter | Owner `Create(name, initial, watcher)` / Client `Open(name)` → `Get(value)` / `Set(value)` → Close; Owner Unlink | [Parameter](examples/parameter_app/README.md), [Owner 소스](examples/parameter_app/src/owner_main.cpp) |
| Service | Server `Create(port)` → `Register<Request, Response>(id, callback)` → Start; Client Create/Call; 종료 시 Server Stop | [Service](examples/service_app/README.md), [Server 소스](examples/service_app/src/server_main.cpp) |
| Action | Server Create 후 Loop에서 Start/PublishFeedback/Succeed 또는 Canceled; Client Create → SendGoal → GetResult, 필요 시 Cancel | [Action](examples/action_app/README.md), [Client 소스](examples/action_app/src/client_main.cpp) |
| Timer | `Create(2.0, callback)`으로 500 ms worker → 종료 시 Stop/join | [Timer](examples/timer_app/README.md), [Element 소스](examples/timer_app/src/timer_main.cpp) |

Topic은 depth=1 기본 최신값, depth=N KEEP_LAST Queue입니다. Subscriber별 독립 Cursor를 사용하며
`ReadNext` 기본 NEXT는 연결 이후 새 값부터, OLDEST는 보관 중인 가장 오래된 값부터 읽습니다.
`ReadLatest`와 Callback은 최신값이며 순차 Cursor를 소비하지 않습니다. `-EAGAIN`은 값 없음/경합/안전 Slot 부족으로
발생할 수 있고 실패 시 Sequence/Cursor는 전진하지 않습니다. `info.missed`는 순차 소비의 누락을 나타냅니다.
Payload의 Timestamp·Validity·측정 Sequence는 Application 책임입니다.

Parameter Set은 전체 값 교체이며 여러 writer의 read-modify-write를 자동 merge하지 않습니다.
Service port/service_id는 Application이 정하며 timeout은 handler 미실행 보장이 아닙니다.
Action Cancel 수락과 CANCELED 완료는 다르고 Feedback은 latest입니다. Timer는 외부 IPC endpoint가 아닙니다.
[IPC 상세 계약](docs/03_IPC_AND_FEATURES.md), [기능 개발 가이드](docs/05_APPLICATION_DEVELOPMENT_GUIDE.md),
[기존 low-level 실행 명령](docs/05_APPLICATION_DEVELOPMENT_GUIDE.md#low-level-examples)을 참조합니다.

## 6. 1 kHz 주기와 ExecutionProfile

`runtime.SetLoopFrequency(1000.0)`은 목표 주기 1 ms를 설정합니다.
**Loop 실행 시간 상한, hard real-time, 정확한 1000 Hz를 보장하지 않습니다.**
지연으로 다음 tick을 넘긴 경우 현재 시각에서 한 주기 뒤로 재예약하며 밀린 Loop를 몰아서 실행하지 않습니다.
긴 I/O·할당·로그·lock 경합은 Application에서 관리해야 합니다.

`ElementSpec::execution_profile`에 다음을 설정할 수 있습니다.

- `policy`: INHERIT(기본), OTHER, FIFO, RR. INHERIT/OTHER의 priority는 0입니다.
- `priority`: FIFO/RR에서는 플랫폼 허용 범위와 RT 권한이 필요합니다. 권한 실패 시 OTHER로 자동 fallback하지 않습니다.
- `cpu_affinity`: 빈 목록은 상속, 지정 시 실제 사용 가능한 CPU/cpuset에 맞춰 선택합니다.
- `lock_memory`: 기본 false. true이면 Runtime이 Setup 전에 `mlockall`을 수행하며 충분한 MEMLOCK 한도가 필요합니다.

실행 환경에서 `ulimit -r`, `ulimit -l`, `taskset -pc $$`로 현재 제한·affinity를 확인합니다.
Profile은 권한을 올리거나 시스템 제한을 변경하지 않습니다. Reset 시 등록된 Profile을 다시 적용합니다.
기존 1 kHz VM Benchmark는 Ubuntu VM/PREEMPT_DYNAMIC 측정이며 **PREEMPT_RT 실장 검증이 아닙니다**.
측정값과 미검증 조건은 [검증 기록](docs/06_VERIFICATION_AND_LIMITATIONS.md#v54-local-integration),
세부 적용 순서는 [Runtime 문서](docs/02_RUNTIME_AND_SUPERVISION.md)를 참고합니다.

## 7. Network와 KCF Tools

Network Action은 기존 Local Action을 호출하며, Application에 별도 Network 등록을 요구하지 않습니다.
`metadata.remote_actions`로 노출을 허용하고 `ControlNode::StartAction` / `CancelAction`을 사용합니다.
Feedback은 bounded latest 방식이며 Cancel 수락과 terminal 취소를 구분합니다.
**연결 소실은 Local Action을 취소하지 않습니다.** Goal/Cancel timeout에는 적용 여부가 불확실할 수 있고,
자동 retry 및 기존 Goal 재연결은 없습니다. [Network 계약과 사용법](docs/08_NETWORK_ARCHITECTURE.md#remote-action--공통-control과-local-action-연결)을 확인하세요.

Remote Topic은 `remote_topics` allowlist와 실제 compatible Subscriber를 확인한 뒤 UDP unicast로 전달합니다.
Control과 분리된 NORMAL Data Plane이며 best-effort latest-data, 기본 최대 50 Hz, 재전송 없음입니다.
기존 Gateway 경로는 수신 측 native TypeDescriptor catalog로 빈 Local proxy를 먼저 준비한 뒤 Subscriber를 생성합니다.
Generic RemoteTopicClient는 정확한 원격 Publisher와 Session을 선택하고 TypeDescriptor Query 및 canonical Payload로
독립 구독합니다. 기존 이름 기반 Gateway 경로와 구분합니다.
Local Topic Format 4와 API는 유지하며 큰 payload는 명시적으로 거부합니다.
Gateway 재시작 시 ownership sidecar로 증명된 죽은 Network proxy만 자동 복구합니다.
일반 Local Publisher·unknown SHM은 삭제하지 않으며, 이전 mapping을 가진 Subscriber는 명시적으로 재연결합니다.
[설정·bootstrap·크기 상한·proxy 수명](docs/08_NETWORK_ARCHITECTURE.md#remote-topic-data-plane)을 확인하세요.

격리된 두 가상 Host의 실제 veth/bridge 통합 시험은
`./tests/network_virtual/run_virtual_network_test.sh`로 실행합니다.
[Virtual LAN 구성·권한·장애 주입·결과](docs/09_VIRTUAL_LAN_TEST.md)를 참고하세요. 일반 빌드와 별도의 opt-in 시험입니다.

v5.3에서 추가된 `ControlConfig::diagnostics_enabled`(기본 OFF)로 허용한 Gateway의 route 상태와 9개 누적 counter를
외부 Observer가 조회할 수 있습니다. `NetworkDiagnosticsClient`는 기존 PeerDiscovery를 빌려 사용하는
동기식 read-only API이며 worker에서 호출합니다. Snapshot은 최대 16 KiB이고 잘림을 명시합니다.
Framework는 counter를 제공하며 Hz 계산·Graph freshness는 Tool 책임입니다.
[Diagnostics API·통신 계약](docs/08_NETWORK_ARCHITECTURE.md#network-diagnostics-v53)을 참고하세요.

### NET56 개발 구현 (버전 5.6.0 적용)

영구 Host ID/NIC 자동 선택, 독립 `kcf_gateway`, systemd 템플릿, Network 재바인딩,
Prefix 기반 Publisher 공개·read-only Topic Observer 및 same-UID Local IPC 조회를 구현했습니다.
Topic별 Rate/100Hz 송신 스케줄러, 새 Route의 과거 명령 차단 Fresh-start,
수신 Commit 기반 `CommandFreshnessMonitor`도 제공합니다. Application 제어 주기와 Network 주기는 독립적입니다.

[현재 옵션·실행 예시·검증 경계](docs/11_NETWORK_V56_DESIGN.md#0-현재-개발-구현과-검증-범위),
[systemd 템플릿](gateway/systemd/README.md), [Local IPC v1](gateway/LOCAL_IPC.md)을 참고하세요.
Gateway 기본 포트는 Discovery UDP 37651, Control TCP/Metadata UDP 39001, Topic UDP 37652입니다.
`--topic-rate /robot/cmd_vel:100`은 기본 50Hz를 대체하지만 권한이나 100Hz 도착을 보장하지 않습니다.
`--topic-allow-prefix /robot/`와 `--topic-fresh-start /robot/cmd_vel`은 각각 공개와 새 Publish 요구 정책입니다.
2026-10-28 사용자 OPI5+ LAN 시험에서 유선·Wi-Fi 50/100/200Hz 수신률은 목표 근접으로 1차 PASS입니다.
[측정값과 Discovery 장애](docs/06_VERIFICATION_AND_LIMITATIONS.md#net56-lan-20261028)는 전체 안정성 보증과 구분합니다.
IGMP Report 후 복구됐지만 정확한 장치 원인은 미확정이며 Graph는 이전 Snapshot이 남을 수 있습니다.
실제 systemd 부팅·자동 재시작, UDP 손실률·p95/p99 지연·장시간 안정성, hard real-time 및 로봇 Fail-safe는 미검증입니다.
서비스는 비활성 상태를 유지했습니다. 당시 200Hz Publisher는 OPI5+ 로컬 시험 코드였습니다. 현재 `kcf_pubsub_publisher`는 기본 1Hz이며
`--hz N`(1~1000)으로 200Hz도 설정할 수 있습니다. 옵션 반영은 현행 커밋의 물리 LAN 재검증 완료를 뜻하지 않습니다.

### 별도 KCF Tools와 검증 범위

[kcf_tools](https://github.com/Nyamkani/kcf_tools)는 별도 repository입니다.
**현재 NET56 연동 대상은 Framework 5.6.0 작업 트리 / KCF Tools 0.14.0**이며 RemoteControlClient,
RemoteTopicClient 및 TypeDescriptor Query를 사용합니다.

- Multi-Host 선택: Local/Remote Host와 정확한 Endpoint를 선택하며, 동일 이름도 Host/Boot/Runtime/등록 ID/Session으로 구분합니다.
- Parameter: Get → 편집 → Set → Readback. Set 성공과 Readback 실패는 별도로 표시합니다.
- Service: Request 편집 → Call → 실제 Response 확인.
- Action: Goal/ACK, 최신 Feedback, Terminal Result, Cancel ACK 및 CANCELED Terminal을 구분합니다.
- Remote Topic: 정확한 Publisher 선택 → Subscribe/Unsubscribe, 최신 Payload·Sequence·수신률 확인.

Dynamic access는 TypeDescriptor와 canonical codec을 사용하며 Primitive Scalar / Fixed Array 범위입니다.
Gateway의 REMOTE 노출 설정과 Get/Set/Service/Goal/Cancel별 독립 Allowlist가 필요합니다.
NET56 Topic Exposure 활성 시 공개 Publisher의 VALUE Descriptor는 Topic Observer에게 별도 Host Allowlist 없이 허용됩니다.
Tools는 Host ID 미설정 시 임시 Topic Observer로 구독할 수 있으며 제어 버튼은 활성화하지 않습니다.
Local Gateway IPC 상태·Peer 조회는 별도 Snapshot으로 표시하며 Direct Observer나 원격 제어 권한을 대체하지 않습니다.
Host ID 및 Allowlist는 암호학적 인증이 아닙니다. 기존 Action 자동 reattach와 변경 명령 자동 retry는 지원하지 않습니다.
Tool은 Gateway나 Payload broker가 아니며 기존 Local 탐색·Topic Echo·Parameter 편집·Service Call도 유지합니다.
Timer endpoint, 일반 Topic Publish, 사용자용 Reset/Launch manager는 제공 기능이 아닙니다.
[Local 지원 범위](docs/04_INTROSPECTION_AND_TOOL.md)는 해당 구현 단계의 기록이며,
현재 Remote UI 사용법은 Tool 저장소의 docs/REMOTE_CONTROL.md에 있습니다.

Diagnostics는 NetworkDiagnosticsClient로 Gateway route/counter를 조회합니다. 수동 Refresh와 Graph freshness
10초는 Tool 정책이며, Tool 자체 수신 구독을 양쪽 Gateway에서 검증한 Route Edge로 추정하지 않습니다.
Source Local Publish Hz, Network Send Hz, Proxy Publish Hz, Application Subscriber 처리율은 서로 다릅니다.
Tool Topic 수신률도 해당 subscription의 관측값이며 Application 처리율이 아닙니다.

**Historical — 2026-10-21 사용자 수동 검증(Framework v5.3 / Tools v0.13):** Orange Pi 5+ ↔ Ubuntu VM Bridged LAN에서 Discovery/Metadata,
Remote Topic 실제 수신(약 45.96 Hz), Parameter Get/Set/Readback, Service Call/Response,
Action Goal/Feedback/SUCCEEDED 및 Cancel ACK/CANCELED를 확인했습니다.
[장치·커밋·시험 설정·문제 이력](examples/network/LAN_TEST_GUIDE.md#physical-lan-2026-10-21)에 결과와
미검증 복구 항목을 구분합니다. 이번 문서 작업에서 시험을 재실행한 결과가 아니며 전체 안정성 검증 완료를 뜻하지 않습니다.
Multi-Host 선택 기능 지원과 실제 다중 Gateway 동시 제어의 물리 LAN 검증 완료도 구별합니다.

**Historical — v0.12 관측 단계:** 최초 기능 반영은 GitHub main commit
[`f4fb447`](https://github.com/Nyamkani/kcf_tools/commit/f4fb4471fc0b03b24663eb20eede8d8491fd87f6)입니다
(현재 Tool HEAD를 뜻하지 않습니다). Remote Host/Application/Element/T/P/S/A metadata와
ONLINE/LOST, compatibility, 읽기 전용 Detail/Host Graph를 제공했으며 Goal/Cancel UI나
외부 Gateway active route/statistics·proxy 판별은 미지원이었습니다.
Tool KCF+Qt 18/18, no-KCF 6/6 PASS와 당시 Tool+Virtual LAN 미검증은 이 단계의 기록입니다.

**Historical — v5.3 Diagnostics-only 연동 단계:** 외부 Diagnostics API와 독립 Virtual LAN Observer·
Qt offscreen E2E가 추가됐으나, 당시 Remote Set/Call/Goal/Cancel UI와 실제 물리 LAN/데스크톱 GUI
수동 검증은 미실행이었습니다. 당시 기록은 현재 Remote Control 지원 범위와 구별합니다.
[기존 검증 출처·측정값·제한](docs/06_VERIFICATION_AND_LIMITATIONS.md#v53-network-diagnostics-검증)은 보존합니다.
현재 기능 설명으로 새 Tool tag/Release 또는 배포 버전을 선언하지 않습니다.


현재 Framework v5.6와 Tool의 신규 통합 시험은 이 문서 작업에서 수행하지 않았습니다.
Builder는 향후 구상이며 현재 구현된 GUI/Launch 기능이 아닙니다.

## 8. Troubleshooting

| 증상 | 먼저 확인할 내용 | 상세 문서 |
| --- | --- | --- |
| Element Setup 실패 | Setup 반환값과 Runtime 로그, 장치·파일 권한, IPC 이름/port 충돌 확인. 획득한 자원만 Shutdown에서 정리. Runtime 초기화 실패와 구분 | [개발 가이드](docs/05_APPLICATION_DEVELOPMENT_GUIDE.md) |
| Supervisor ERROR | GetApplicationError의 최초 장애, GetErrorHistory의 후속 장애, 실행 경로/인자/timeout 확인. Setup==0만 보고 RUNNING으로 판단하지 않음. 원인 제거 후 명시적 RequestReset | [Runtime/Supervision](docs/02_RUNTIME_AND_SUPERVISION.md#supervisor-states-and-reset) |
| RT Scheduling 권한 오류 | policy/priority, RT limit, 허용 CPU, MEMLOCK 한도 확인. exec/Setup 전 실패 여부를 구분하며 자동 fallback을 기대하지 않음 | [ExecutionProfile](docs/02_RUNTIME_AND_SUPERVISION.md) |
| SHM 재생성 후 값 정지 | 기존 Subscriber는 이전 mapping을 유지. 로컬 읽기/worker를 종료하고 Close/Create. Dynamic reader의 -ESTALE은 Close/Open. 다른 소유자의 SHM을 일괄 삭제하지 않음 | [명시적 재연결](docs/05_APPLICATION_DEVELOPMENT_GUIDE.md#external-observer의-명시적-shm-재연결) |
| Network Discovery 지연/LOST | interface/IP/port, 방화벽 UDP 37651(기본 설정), 중복 Host ID Gateway, Peer compatibility/session 확인. Endpoint metadata와 실제 전송·권한 확인을 구분 | [LAN 가이드](examples/network/LAN_TEST_GUIDE.md), [Network 계약](docs/08_NETWORK_ARCHITECTURE.md) |

Composition Observer의 `r` + Enter는 해당 예제에 구현된 명시적 재연결 명령이며 범용 Framework CLI가 아닙니다.
과거 방화벽·중복 Gateway 문제와 간헐적 실패/재실행 기록은 아래 검증 문서에 보존합니다.

## 호환성과 주요 제한 사항

- **Topic SHM Format 4 / Parameter Format 3**. 이전 Topic Format 3과 바이너리 비호환이므로 Publisher/Subscriber 및 Tool을 호환 Core로 재빌드해야 합니다. 이전 SHM은 자동 삭제·변환하지 않습니다.
- Local IPC는 같은 Linux 호스트와 호환 Payload ABI가 필요합니다. Payload는 fixed-size trivially-copyable이며 pointer/heap ownership을 포함하지 않습니다. Network는 별도 type/schema/encoding 검증과 명시적 노출 설정을 사용합니다.
- Remote Topic은 유실 없는 Queue 복제가 아닌 best-effort 최신값 전달입니다. 기존 Action reattach·변경 명령 자동 retry·인증/암호화는 미지원입니다. Remote TypeDescriptor Query는 Primitive Scalar / Fixed Array 범위이며 임의의 동적 타입을 지원하지 않습니다.
- 물리 LAN 1차 기능 검증과 별개로 Gateway 단독 재시작, Tools 단독 재시작, 일시적 LAN 장애 후 자동 복구 및 다중 Gateway 동시 제어 안정성은 추가 검증이 필요합니다.
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
- [NET56 현재 개발 구현·원 설계](docs/11_NETWORK_V56_DESIGN.md)
- [Network Deployment Policy v1.0](docs/10_NETWORK_DEPLOYMENT_POLICY.md) — 다중 장치·포트·Identity 운영 및 향후 Debian 패키징 정책

공개 문서는 `docs/`에 포함됩니다. `docs/instructions/`만 Git ignore된 로컬 개발 지침 디렉터리입니다.

현재 Remote 기능과 향후 배포 자동화는 별개입니다.
[Deployment Policy](docs/10_NETWORK_DEPLOYMENT_POLICY.md)의 당시 미구현 기록은 보존합니다. 영구 ID API와 Gateway/systemd 템플릿은 현재 구현됐고,
network.conf 로더·Debian 패키징·실제 systemd 운영 검증은 아직 완료되지 않았습니다.
