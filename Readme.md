# KSS Control Framework (KCF)

KCF는 Linux 기반 제어 시스템에서 반복되는 프로세스 실행 구조, lifecycle,
IPC 및 command/state 구조를 공통화하기 위한 재사용 가능한 제어 Application Framework입니다.
로봇, 로봇팔, 산업 장비, 센서·액추에이터 및 SBC 기반 제어 시스템에 적용하는 것을 목표로 합니다.

Linux/POSIX API의 반복 사용을 얇게 감싸고, 프로세스와 통신 구조가 코드에서
명확하게 보이도록 설계합니다. ROS를 재구현하지 않으며, 매카넘 로봇은 향후 첫 Reference Application입니다.

## 현재 상태: v4.1

v3.0 hardening 완료 상태를 v4.0 기준으로 두고, v4.1에서 Mecanum M-0 데이터 헤더와 Runtime lifecycle/error 계약을 반영했습니다.

| 버전 | 완료 범위 |
| --- | --- |
| v1.0 | ProcessElement / ProcessRuntime / Process / Bringup 실행 기반 |
| v2.0 | Topic / Timer / Service / Runtime Parameter / Action 통신 기반 |
| v3.0 | Multi-Element supervision, ERROR/SAFE, 명시적 Reset, Supervisor loss 및 SHM crash hardening |
| v4.0 | Mecanum 데이터 헤더 추가 전 Framework 기준 상태 |
| v4.1 | M-0 Application data contract, Setup 실패 정리 및 Loop fatal error 반환 |

v3.0은 최초 오류를 유지하고, 사용자 Reset으로 전체 process generation을 교체한 뒤
모든 Runtime이 RUNNING일 때만 정상 운전으로 복귀합니다. 자동 재시작은 하지 않습니다.

| 기능 | 역할 | 실행 / 전달 방식 |
| --- | --- | --- |
| Process Framework | Element lifecycle과 외부 프로세스 관리 | ProcessRuntime, Process, Bringup |
| System Supervision | 여러 Element의 초기화·건강 상태·최초 오류 관리 | Runtime Supervision, SystemStatus, 명시적 Reset |
| Topic | 센서·상태·연속 명령의 최신값 | Shared Memory, 1 Publisher : N Subscriber |
| Timer | 독립적인 주기 callback | Timer별 worker, steady clock |
| Service | 짧은 단발 Request / Response | localhost UDP, timeout / retry / 중복 응답 캐시 |
| Runtime Parameter | 실행 중 현재 설정값 유지·변경 | Shared Memory, Get / Set / 선택적 변경 callback |
| Action | 시간이 걸리는 작업의 Goal / Feedback / Result / Cancel | 명령·결과는 Service, 상태·Feedback은 Topic |

실제 Device Driver, 매카넘 애플리케이션, UI, startup config loader, discovery,
자동 재시작 및 원격 네트워크 gateway는 현재 구현 범위에 포함하지 않습니다.

## 설계와 책임 분리

Backend가 제어·상태·판단·FSM·안전 로직을 소유하며 UI 없이 독립 동작하도록 설계합니다.
Frontend는 상태 표시, 사용자 입력, 데이터 송수신을 담당합니다. 현재 UI 구현은 없습니다.

```text
Bringup / Supervisor
    └─ kcf::Process로 Element Process 생성·종료·회수
        └─ ProcessRuntime
            └─ ProcessElement: Setup / Loop / Shutdown
                └─ 기능별 Component: Driver / Algorithm / Interface
```

Bringup은 Element 내부 기능을 직접 호출하지 않습니다.
현재 DummyElement는 로그만 출력하며 scheduler와 signal 처리는 Runtime이 담당합니다.

정상 lifecycle은 다음과 같습니다.

```text
STOPPED → STARTING → Setup() → RUNNING → Loop() 반복
        → Stop Request → STOPPING → Shutdown() → STOPPED
```

`Setup()`과 `Loop()`는 `int`를 반환합니다. 0은 성공/정상 cycle, non-zero는 lifecycle을 종료할 오류입니다.
가능하면 negative errno를 사용하며, Runtime은 양수 오류도 실패로 취급합니다. 일시적인 오류의 retry/recovery는
Element에서 처리하고 정상 운전을 계속할 수 있을 때만 Loop에서 0을 반환합니다.

Setup 호출이 시작되면 반환 오류나 예외가 발생해도 Runtime은 Shutdown을 정확히 한 번 호출합니다.
Shutdown은 부분 초기화 상태에서도 안전하게 자원을 정리해야 하며 Setup에서 직접 호출하지 않습니다.
Runtime 내부 초기화·supervision 시작 실패로 Setup을 호출하지 않았다면 Shutdown도 호출하지 않습니다.
Loop 오류 cycle은 heartbeat를 증가시키지 않고 ERROR → Shutdown → 오류 반환으로 종료합니다.
Setup/Loop 예외는 `-EFAULT`이며 Shutdown 예외가 기존 오류를 덮어쓰지 않습니다.
이미 latch된 Supervisor loss의 우선순위와 정상 signal/RequestStop의 STOPPING → Shutdown → STOPPED 흐름은 유지합니다.
`Shutdown()`은 기본 빈 구현을 제공합니다.

**기존 Element 이식:** `void Loop() override`를 `int Loop() override`로 바꾸고 정상 경로에서 `return 0;`을 추가해야 합니다.
새 API로 모든 Element를 다시 빌드해야 합니다. Unix process exit status는 음수 int를 그대로 보존하지 않으므로
정확한 errno 확인에는 `ProcessRuntime::Run()` 반환값 또는 Runtime status를 사용합니다.

## System Supervision / Fault Management

Multi-Element Supervisor는 모든 ProcessRuntime이 RUNNING을 응답한 뒤에만 Application을 RUNNING으로 전환합니다.
Process 종료, Runtime ERROR, 상태 응답 timeout, Loop heartbeat 정지를 구분하며 최초 오류를 latch합니다.
살아 있는 Element는 SystemStatus ERROR를 받아 application의 SAFE 동작을 수행합니다.

```text
INITIALIZING → RUNNING → ERROR
                         ↓ 명시적 RequestReset()
                     RESETTING
                         ↓ 전체 새 Runtime RUNNING 확인
                      RUNNING
```

`Bringup::RequestReset()`은 ERROR에서만 Reset 요청을 기록합니다. Supervisor가 기존 Element 전체를
종료·회수하고 같은 설정으로 새 process generation을 시작합니다. 모든 새 Runtime이 RUNNING인 경우에만
오류를 해제합니다. Reset 실패 시 ERROR를 유지하며 다음 명시적 요청을 기다립니다.
자동 restart, 일부 Element만 재시작, Reset용 UI·network command는 제공하지 않습니다.

종료는 SIGTERM을 먼저 보내고 설정한 timeout 이후 살아 있는 child에 SIGKILL을 보낸 뒤 회수합니다.
이 강제 정리는 Reset 또는 전체 종료에만 사용하며, health fault 감지 즉시 child를 강제 종료하지 않습니다.
기존 generation이 모두 사라진 뒤 새 owner의 Create가 dead owner metadata를 확인하여 stale IPC를 복구합니다.
SystemStatus 전용 저장소는 Reset 동안 유지하며, 기존 IPC mapping의 hot reconnect는 제공하지 않습니다.
Dead owner의 SHM을 Open하면 `-EAGAIN`을 반환하므로 새 owner의 Create 이후 재시도합니다.

SafeElement 예제는 초기 output=0으로 시작하고 SystemStatus RUNNING 이후에만 정상 output을 허용합니다.
ERROR를 받은 generation의 SAFE latch는 이후 RUNNING snapshot으로 자동 해제되지 않습니다.
KCF의 software supervision/SAFE와 Device의 communication watchdog·hardware safety는 서로 다른 책임입니다.
KCF나 SIGKILL이 Device watchdog을 대체하지 않습니다. Linux kernel uninterruptible sleep(D state)은
SIGKILL 이후에도 userspace에서 bounded 종료를 보장할 수 없습니다.

### Supervisor Loss Hardening — Phase 3-8

Supervised Element는 기존 RuntimeStatusRequest로 Supervisor 생존을 확인합니다.
Socket 연결이 끊기거나 유효 요청이 2초간 없으면 Runtime이 종료를 요청하고,
main 경로에서 Shutdown 후 non-zero로 종료합니다. Standalone 실행에는 이 감시를 적용하지 않습니다.
정상 Supervisor 종료와 Reset의 SIGTERM은 기존 정상 종료로 처리합니다.

Setup/Loop가 무한 block되면 worker가 감지해도 main 경로의 Shutdown을 강제할 수 없습니다.
Device watchdog과 외부 process manager가 필요하며, KCF 내부 자동 restart나 systemd 설정은 추가하지 않습니다.

### Persistent SystemStatus Hardening — Phase 3-9A

SystemStatus는 일반 Topic과 분리된 robust shared current-state 저장소를 사용합니다.
`SystemStatusPublisher` / `SystemStatusSubscriber`가 기존 SharedParameter 동기화·복구를 재사용하여,
reader crash가 Reset을 넘어 영구 reader pin으로 누적되는 구조를 제거했습니다.
Subscriber는 초기/current snapshot 조회와 lock 밖 callback을 제공하며, Reset 동안 같은 저장소를 유지합니다.
일반 고속 Topic의 triple buffer·1:N·latest-value 동작은 그대로입니다.

새 저장소 이름은 `/kcf/system/status/state`입니다. 기존 Topic 형식의 `/kcf/system/status`와 혼용하거나
자동 migration하지 않습니다. 같은 객체에서 reader crash 20회, inode·FD 유지 및 Reset 회귀를 검증했습니다.

### Incomplete SHM Recovery Hardening — Phase 3-9B

초기화 도중 owner가 종료된 SHM에서 Open과 replacement Create가 경합하던 결함을 수정했습니다.
Topic·Parameter의 Open은 짧은 header 또는 `initialized=0`이면 flock 없이 `-EAGAIN`을 반환합니다.
Caller는 초기화·복구 완료 후 제한된 횟수나 시간 안에서 재시도해야 합니다. Live initializer 보호와 잠금 후 검증은 유지하며,
일반 Topic Publish/Read 및 Parameter Get/Set 경로는 변경하지 않았습니다.

수정 전 실제 잠금 순서에서 replacement Create의 `-EEXIST` 실패를 재현했습니다.
수정 후 Topic·Parameter 각각 400회 동시 시작과 4개 결정적 잠금 순서 테스트(각 404회)를 통과했습니다.
Reset 중 initializer crash 후 새 generation의 값 수신과 전체 Runtime RUNNING 복귀도 확인했습니다.

## 통신과 실행 책임

Topic은 최신 snapshot을 전달하며 느린 Subscriber는 중간 값을 건너뛸 수 있습니다.
Service는 프로세스당 하나의 서버 UDP port에서 명시적인 service ID로 요청을 분기합니다.
Action도 이 ServiceServer를 재사용하며 별도 Action 서버 port나 작업 실행 worker를 만들지 않습니다.

Action의 공통 상태는 다음과 같습니다. 실제 작업은 application Loop 또는 Timer callback이 수행합니다.

```text
IDLE / terminal → ACCEPTED → RUNNING → SUCCEEDED / FAILED
ACCEPTED / RUNNING → cancel_requested → application의 Canceled() → CANCELED
```

ActionServer당 active Goal은 하나이며 terminal 이후 다음 Goal을 수락할 수 있습니다.
Cancel 요청 수락과 취소 완료는 구분합니다. Feedback은 latest-value이고,
최종 Result는 다음 Goal 수락 전까지 보존하여 GetResult Service로 조회합니다.

Runtime Parameter는 현재 값과 version을 함께 관리합니다. 변경 callback은 선택 사항이며
느린 watcher는 중간 변경을 생략할 수 있습니다. 설정값의 디스크 영속 저장은 제공하지 않습니다.

Subscriber, Timer, ServiceServer 및 Parameter watcher의 callback은 각 worker에서 실행됩니다.
사용자 공유 상태는 application에서 동기화하고, callback은 종료 시 join이 가능하도록 반환해야 합니다.
Shared Memory의 Close와 owner의 Unlink는 구분되며 정상 종료 시 owner가 명시적으로 정리합니다.

현재 통신은 같은 Linux 호스트와 동일한 타입 layout / ABI를 전제로 합니다.
전달 타입은 trivially copyable이어야 하며 pointer나 heap 소유 멤버를 포함하지 않습니다.
Service의 중복 보호는 최근 64개 응답 캐시, Action의 추가 보호는 현재/직전 Goal 범위입니다.
서버 재시작 이후의 영구 중복 방지는 제공하지 않습니다.
v3.0은 Topic/Parameter의 robust 동기화 복구와 controlled generation recovery를 제공합니다.
Crash로 남은 reader pin은 개별 회수하지 않으며 전체 generation 교체로 복구합니다.

## Mecanum Reference Application — M-0

`applications/mecanum/data/`는 Framework와 분리된 Application 전용 IPC 데이터 계약입니다.

| 헤더 | 타입 / 의미 |
| --- | --- |
| `common.hpp` | `SampleHeader`: producer sequence와 monotonic microsecond timestamp |
| `motor.hpp` | 3축 `VelocityCommand`, `OdometryData` |
| `imu.hpp` | `ImuData`: 가속도(g), 각속도(rad/s), Euler(rad), 자기장 raw, valid mask |
| `lidar.hpp` | `FixedLidarScan<PointT, MaxPoints>`: 제품 독립적인 고정 용량 container |

타입은 trivially copyable·standard layout 조건을 검증하며 heap ownership을 포함하지 않습니다.
M-0에는 실제 Element·Driver·제품별 LiDAR payload 구현이 없습니다.

## 저장소 구조

```text
.
├─ CMakeLists.txt
├─ Readme.md
├─ Changelog.md
├─ applications/mecanum/data/
├─ kcf/
│  ├─ CMakeLists.txt
│  ├─ include/kcf/
│  │  ├─ process/
│  │  ├─ ipc/
│  │  ├─ timer/
│  │  ├─ service/
│  │  ├─ parameter/
│  │  ├─ action/
│  │  └─ system/
│  └─ src/
├─ examples/
│  ├─ dummy/
│  ├─ topic/
│  ├─ timer/
│  ├─ service/
│  ├─ parameter/
│  ├─ action/
│  ├─ integration/
│  ├─ supervisor/
│  └─ recovery/
└─ bringup/
```

각 예제는 자체 CMakeLists.txt와 소스를 가집니다.
`docs/`는 Git에서 제외된 로컬 개발 문서 디렉터리이며 clone에 포함되지 않습니다.

## 빌드

Linux 환경, CMake 3.16 이상, C/C++ 컴파일러(C++17 지원), POSIX Threads가 필요합니다.
저장소 루트에서 실행합니다.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

| Target | 역할 | 기본 빌드 결과 |
| --- | --- | --- |
| `kcf_core` | 공통 Framework 라이브러리 | `build/kcf/libkcf_core.a` |
| `kcf_dummy` | Dummy Element 실행 파일 | `build/examples/dummy/kcf_dummy` |
| `kcf_bringup` | Supervisor 실행 파일 | `build/bringup/kcf_bringup` |
| `kcf_topic_pub`, `kcf_topic_sub` | Topic 발행·구독 예제 | `build/examples/topic/` |
| `kcf_timer_test` | Timer 동작 검증 | `build/examples/timer/kcf_timer_test` |
| `kcf_service_server`, `kcf_service_client` | Service 요청·응답 예제 | `build/examples/service/` |
| `kcf_parameter_owner`, `kcf_parameter_client` | Runtime Parameter 예제 | `build/examples/parameter/` |
| `kcf_action_server`, `kcf_action_client` | Count Action 예제 | `build/examples/action/` |

추가 검증 target은 `examples/supervisor/`의 `kcf_supervisor_normal`, `kcf_supervisor_crash`,
`kcf_supervisor_safe`, `kcf_supervisor_runtime_test`, `kcf_runtime_supervision_test`, `kcf_reset_test`,
`kcf_runtime_lifecycle_test`, `kcf_system_status_recovery_test`와
`examples/recovery/`의 `kcf_topic_recovery_test`, `kcf_parameter_recovery_test`,
`kcf_incomplete_recovery_test`입니다.
Integration 예제는 `kcf_integration_backend`, `kcf_integration_client`입니다.

최상위 CMake에서 하위 영역을 명시적으로 등록합니다. `build/`는 Git 추적에서 제외합니다.

## 실행

### Dummy 단독 실행

```sh
./build/examples/dummy/kcf_dummy
```

`[Dummy] Setup`이 한 번 출력되고, `[Dummy] Loop`가 약 500ms 간격으로 반복됩니다.
Ctrl+C를 입력하면 `[Dummy] Shutdown`이 한 번 출력되고 종료합니다.

### Bringup 통합 실행

```sh
./build/bringup/kcf_bringup ./build/examples/dummy/kcf_dummy
```

여러 Element는 `--next`로 구분합니다.

```sh
./build/bringup/kcf_bringup ./build/examples/supervisor/kcf_supervisor_normal --element-name A --next ./build/examples/supervisor/kcf_supervisor_normal --element-name B
```

각 executable 뒤에 `--startup-timeout-ms`, `--health-timeout-ms`, `--shutdown-timeout-ms`를 지정할 수 있습니다.
기본값은 각각 5000 / 2000 / 1500ms입니다. Health timeout은 정상 Loop 간격보다 충분히 크게 설정합니다.
Child가 예기치 않게 종료되면 Application ERROR를 유지하고 자동 재시작하지 않습니다.
Ctrl+C 또는 SIGTERM으로 전체 child를 종료·회수합니다. Ctrl+C는 같은 터미널의 child에도 전달될 수 있습니다.
Reset 제어는 public `RequestReset()` API로 제공하며 CLI Reset 명령은 없습니다.

### Fault / Recovery 검증

다음 검증은 child에 의도적으로 crash·stall·SIGSTOP을 유도합니다. 다른 KCF Supervisor와 동시에 실행하지 않습니다.
Python 검증에는 Python 3가 필요합니다.

```sh
./build/examples/supervisor/kcf_runtime_lifecycle_test
./build/examples/supervisor/kcf_runtime_supervision_test ./build/examples/supervisor/kcf_supervisor_runtime_test
python3 examples/supervisor/runtime_health_checks.py build
python3 examples/supervisor/supervisor_loss_checks.py build
python3 examples/supervisor/reset_checks.py build
./build/examples/supervisor/kcf_system_status_recovery_test
./build/examples/recovery/kcf_topic_recovery_test
./build/examples/recovery/kcf_parameter_recovery_test
./build/examples/recovery/kcf_incomplete_recovery_test topic
./build/examples/recovery/kcf_incomplete_recovery_test parameter
```

Incomplete recovery 테스트는 transport별로 크기 0, 짧은 header, 미완료 header,
전체 크기이나 초기화 미완료인 객체를 검증합니다. Consumer retry는 2초, 실행 전체는 60초로 제한합니다.
`reset_checks.py`는 반복 Reset과 실패·종료 처리 외에 Topic/Parameter initializer crash 후 복구도 포함합니다.

### Topic

각 명령을 별도 터미널에서 실행하며 Publisher를 먼저 시작합니다.

```sh
./build/examples/topic/kcf_topic_pub 1000
./build/examples/topic/kcf_topic_sub 0 1000
```

Publisher 인자는 Hz입니다. Subscriber 인자는 callback 지연(ms), 로그 출력 간격입니다.
느린 수신 확인에는 `kcf_topic_sub 350 1`을 사용합니다. Ctrl+C로 종료합니다.

### Timer

```sh
./build/examples/timer/kcf_timer_test
```

2 Hz / 100 Hz 주기, Stop, overrun 및 Timer에서 Topic 발행을 검증한 뒤 자동 종료합니다.

### Service

서버를 먼저 실행하고 별도 터미널에서 클라이언트를 실행합니다.

```sh
./build/examples/service/kcf_service_server 22000
./build/examples/service/kcf_service_client 22000 1000
```

하나의 port에서 Add와 SetValue를 제공하며 클라이언트는 연속 호출 1,000회를 검증합니다.
클라이언트는 자동 종료하고 서버는 Ctrl+C로 종료합니다.

### Runtime Parameter

Owner를 먼저 실행한 뒤 별도 터미널에서 client를 실행합니다.

```sh
./build/examples/parameter/kcf_parameter_owner
./build/examples/parameter/kcf_parameter_client
```

Client는 현재 설정을 읽고 kp/ki/kd/mode를 변경한 뒤 종료합니다.
변경 감시는 `kcf_parameter_client watch`로 실행합니다. Owner와 watcher는 Ctrl+C로 종료합니다.

### Action

서버를 먼저 실행하고 다른 터미널에서 클라이언트를 실행합니다.

```sh
./build/examples/action/kcf_action_server 22010
./build/examples/action/kcf_action_client 22010 20
```

서버 application loop가 100ms마다 count를 증가시키며, 클라이언트는 최종 `final_count=20`을
GetResult로 확인한 뒤 종료합니다. 같은 서버에 다음 클라이언트를 순서대로 실행할 수 있습니다.

```sh
# 느린 Feedback callback(350ms)에서도 최종 Result 확인
./build/examples/action/kcf_action_client 22010 20 350

# 긴 Goal을 실행하고 350ms 후 취소 요청
./build/examples/action/kcf_action_client 22010 100 0 350
```

인자는 `[port] [target_count] [callback_delay_ms] [cancel_after_ms]`입니다.
Goal/Cancel/GetResult ID는 100/101/102, 상태 Topic은 `/kcf_test_count_action/status`입니다.
작업 검증 후 서버를 Ctrl+C로 종료하면 상태 Topic을 정리합니다.

## 검증 현황

다음은 개발 과정에서 수행한 검증 결과이며, 모든 검증이 저장소의 자동 테스트 target으로
등록되어 있는 것은 아닙니다.

| 범위 | 확인한 내용 |
| --- | --- |
| v1.0 Process Framework | clean build, Dummy 2 Hz, Ctrl+C/SIGTERM, Bringup child 생성·회수, zombie 부재 |
| v2.0 Build | 기존 target 및 모든 Phase 2 예제의 Debug clean build 성공 |
| Topic | 다중 프로세스 snapshot 무결성, 느린 수신, 초기화 경합 및 반복 종료 |
| Timer | 2 Hz / 100 Hz, Stop, overrun, Topic 발행·수신 300회 |
| Service | 연속 호출 1,000회, timeout/retry, 응답 검증, 중복 요청 및 port 충돌 |
| Parameter | 다중 프로세스 Set 4,000회, 일관된 값/version, 느린 watcher, lifecycle 100회 |
| Action | 성공·실패·취소 FSM, Busy/잘못된 ID, 캐시 퇴출 후 중복 Goal 방지, 연속 Goal |
| Action 동시성·정리 | 동시 Feedback 2,000회, 느린 Feedback과 독립적인 Result 조회, lifecycle 25회 FD·SHM 정리 |

v3.0 Phase 3-7에서 다음을 재검증했습니다.

- STARTING/RUNNING barrier, 네 가지 fault, 최초 origin·secondary failure 및 SAFE 유지.
- Fault 종류를 섞은 Reset 10회, Topic/Parameter stale owner 복구, 실패 후 명시적 재Reset.
- Reset 중 operational gate 및 사용자 종료 우선 처리, SIGKILL/reap, FD·child·SHM 정리.
- 20 Element의 단일 Supervisor monitoring context와 control Loop 진행.
- Topic latest-value/1:N, Timer, Service, Parameter transaction/watcher, Action EAGAIN, Integration 10 lifecycle.

Phase 3-8~3-9B hardening까지 다음 검증을 추가로 완료했습니다. 결과는 모두 **PASS**입니다.

| 단계 | 확인한 내용 |
| --- | --- |
| Phase 3-8 | Supervisor SIGKILL/SIGSTOP, supervised Element 자체 Shutdown·reap, standalone 영향 없음 |
| Phase 3-9A | 동일 SystemStatus 객체에서 dead-reader 20회 복구, callback 재진입·join, Reset 동안 inode·최초 오류 유지 |
| Phase 3-9B | Topic·Parameter 각각 404회 Open/Create race, live owner·unknown header 보호, stale attach 부재, initializer crash 후 Reset |
| 최종 회귀 | Debug clean build, Topic/Parameter recovery, 1 MiB transaction, Reset 10회, Runtime supervision, 20 Element·1 kHz, 전체 통신 및 Integration 10 lifecycle |
| 자원 정리 | FD baseline 유지, 잔여 process·zombie·SHM 없음, UDP 포트 재사용 |

1 kHz 검증은 해당 테스트 환경에서의 Loop 진행 확인이며 hard real-time 보장을 의미하지 않습니다.

v4.1 Runtime lifecycle 테스트에서는 Setup 성공·오류·예외, Loop 오류·예외, 양수/음수 오류 보존,
실패 cycle의 heartbeat 미증가, Shutdown 예외의 우선순위, Setup 전 내부 실패의 cleanup 생략을 확인했습니다.
직접 Run 반환값과 실제 supervision status를 함께 검사하며 부분 초기화 자원과 FD 정리도 검증합니다.
C++17 clean build, 기존 Runtime·20 Element/1 kHz·Reset 10회·Supervisor loss, 통신 및 Integration 10 lifecycle 회귀까지 **PASS**입니다.

## 개발 기준 문서

로컬 `docs/KCF_ARCHITECTURE_PLAN.md`와 `docs/KCF_CODEX_INSTRUCTION.md`를 설계 및 개발 기준으로
사용합니다. 두 문서는 Git ignore 상태로 유지합니다.

공개 개발 이력은 [Changelog](Changelog.md)의 v0.1~v4.1 항목을 참고하세요.
