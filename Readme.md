# KSS Control Framework (KCF)

KCF는 Linux 기반 제어 시스템에서 반복되는 프로세스 실행 구조, lifecycle,
IPC 및 command/state 구조를 공통화하기 위한 재사용 가능한 제어 Application Framework입니다.
로봇, 로봇팔, 산업 장비, 센서·액추에이터 및 SBC 기반 제어 시스템에 적용하는 것을 목표로 합니다.

Linux/POSIX API의 반복 사용을 얇게 감싸고, 프로세스와 통신 구조가 코드에서
명확하게 보이도록 설계합니다. ROS를 재구현하지 않으며, 매카넘 로봇은 향후 첫 Reference Application입니다.

## 현재 상태: v2.0

v1.0의 Process Execution Framework 위에 Phase 2-1~2-5의 통신 및 실행 보조 기능을
추가했습니다. v2.0은 Topic, Timer, Service, Runtime Parameter, Action의 구현과 검증을 포함합니다.

| 기능 | 역할 | 실행 / 전달 방식 |
| --- | --- | --- |
| Process Framework | Element lifecycle과 외부 프로세스 관리 | ProcessRuntime, Process, Bringup |
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

`Setup()`은 성공 시 0, 실패 시 non-zero를 반환합니다.
Setup 실패 시 Runtime은 `ERROR` 상태로 종료하며 `Shutdown()`을 자동 호출하지 않습니다.
`Shutdown()`은 기본 빈 구현을 제공하므로 정리 작업이 필요한 Element만 재정의합니다.

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
서버 재시작 이후의 영구 중복 방지나 프로세스 강제 종료 중 공유 동기화 복구는 제공하지 않습니다.

## 저장소 구조

```text
.
├─ CMakeLists.txt
├─ Readme.md
├─ Changelog.md
├─ kcf/
│  ├─ CMakeLists.txt
│  ├─ include/kcf/
│  │  ├─ process/
│  │  ├─ ipc/
│  │  ├─ timer/
│  │  ├─ service/
│  │  ├─ parameter/
│  │  └─ action/
│  └─ src/
├─ examples/
│  ├─ dummy/
│  ├─ topic/
│  ├─ timer/
│  ├─ service/
│  ├─ parameter/
│  └─ action/
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

Bringup은 첫 번째 인자로 받은 실행 파일 경로를 사용합니다.
정상 실행과 종료 시 대표적인 출력은 다음과 같습니다.

```text
[Bringup] Dummy process started
[Dummy] Setup
[Dummy] Loop
[Dummy] Loop
...
```

Ctrl+C 입력 후:

```text
[Bringup] Sending SIGTERM to dummy
[Dummy] Shutdown
[Bringup] Dummy process exited
[Bringup] Shutdown complete
```

Ctrl+C는 같은 터미널의 child에도 전달될 수 있어 로그 순서나 SIGTERM 전달 로그의
출력 여부는 종료 타이밍에 따라 달라질 수 있습니다.
Bringup은 실행 중인 child에 종료를 요청하고, 이미 종료한 child도 회수 상태를 확인합니다.
child가 먼저 종료하면 이를 감지하고 재시작 없이 종료합니다.

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

Phase 2-5 완료 시 Topic / Timer / Service / Parameter 회귀 검증도 모두 통과했습니다.

## 개발 기준 문서

로컬 `docs/KCF_ARCHITECTURE_PLAN.md`와 `docs/KCF_CODEX_INSTRUCTION.md`를 설계 및 개발 기준으로
사용합니다. 두 문서는 Git ignore 상태로 유지합니다.

공개 개발 이력은 [Changelog](Changelog.md)의 v0.1~v1.0 및 v2.0 항목을 참고하세요.
