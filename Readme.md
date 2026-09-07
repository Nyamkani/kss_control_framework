# KSS Control Framework (KCF)

KCF는 Linux 기반 제어 시스템에서 반복되는 프로세스 실행 구조, lifecycle,
IPC 및 command/state 구조를 공통화하기 위한 재사용 가능한 제어 Application Framework입니다.
로봇, 로봇팔, 산업 장비, 센서·액추에이터 및 SBC 기반 제어 시스템에 적용하는 것을 목표로 합니다.

Linux/POSIX API의 반복 사용을 얇게 감싸고, 프로세스와 통신 구조가 코드에서
명확하게 보이도록 설계합니다. ROS를 재구현하지 않으며, 매카넘 로봇은 향후 첫 Reference Application입니다.

## 현재 상태: v0.7

Process Execution Framework의 구현과 Linux 통합 검증을 완료했습니다.
기존 개발 단계 Phase 1-1~1-7은 변경 이력에서 각각 v0.1~v0.7로 표기합니다.

- `ProcessElement`: 사용자 Element의 `Setup()` / `Loop()` / `Shutdown()` 인터페이스
- `ProcessRuntime`: 프로세스 내부 lifecycle, 주기 실행, SIGINT/SIGTERM 처리
- `Process`: `fork()` / `exec()` / SIGTERM / `waitpid()` wrapper
- `kcf_dummy`: 2 Hz로 실행하는 최소 Element 예제
- `kcf_bringup`: Dummy 하나를 생성하고 종료·회수를 관리하는 Supervisor

Topic, SharedChannel, Service, Action communication, Parameter, 실제 장치 제어와
매카넘 애플리케이션은 아직 구현하지 않았습니다.
자동 재시작, 범용 프로세스 관리, 종료 timeout 및 강제 종료도 현재 범위에 포함하지 않습니다.

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

## 저장소 구조

```text
.
├─ CMakeLists.txt
├─ Readme.md
├─ Changelog.md
├─ docs/
│  ├─ KCF_ARCHITECTURE_PLAN.md
│  └─ KCF_CODEX_INSTRUCTION.md
├─ kcf/
│  ├─ CMakeLists.txt
│  ├─ include/kcf/process/
│  └─ src/
├─ examples/dummy/
│  ├─ CMakeLists.txt
│  ├─ include/dummy/
│  └─ src/
└─ bringup/
   ├─ CMakeLists.txt
   ├─ include/bringup/
   └─ src/
```

## 빌드

Linux 환경, CMake 3.16 이상, C/C++ 컴파일러(C++17 지원), POSIX Threads가 필요합니다.
저장소 루트에서 실행합니다.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

| Target | 역할 | 기본 빌드 결과 |
| --- | --- | --- |
| `kcf_core` | 공통 Process Framework 라이브러리 | `build/kcf/libkcf_core.a` |
| `kcf_dummy` | Dummy Element 실행 파일 | `build/examples/dummy/kcf_dummy` |
| `kcf_bringup` | Supervisor 실행 파일 | `build/bringup/kcf_bringup` |

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

## 검증 현황

v0.7에서 clean configure/build, Dummy 단독 실행, Bringup 통합 실행을 검증했습니다.
Loop 간격은 약 498~503ms였으며, Setup과 Shutdown은 각각 한 번 호출됐습니다.
Ctrl+C, Bringup에만 전송한 SIGTERM, child 선종료 상황을 확인했고 종료 후 child와 zombie가 남지 않았습니다.
정상 종료 코드는 0이며, child 선종료 감지 시 Bringup은 1을 반환합니다.

## 개발 기준 문서

- [Architecture Plan](docs/KCF_ARCHITECTURE_PLAN.md): 설계, 계층별 책임, 향후 통신 모델과 개발 범위
- [Codex Development Instruction](docs/KCF_CODEX_INSTRUCTION.md): 작업 범위와 개발 규칙
- [Changelog](Changelog.md): v0.1~v0.7 주요 변경 및 검증 이력
