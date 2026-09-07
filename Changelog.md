# Changelog

KSS Control Framework(KCF)의 주요 개발 및 검증 이력입니다.
기존 Phase 1-1~1-7을 각각 v0.1~v0.7로 표기하며, 최신 버전부터 기록합니다.
이 문서의 버전 표기는 개발 이력 구분이며 Git tag 또는 배포 생성 여부를 의미하지 않습니다.

## v0.7 — Process Execution Framework 통합 검증

- 기존 build 디렉터리를 제거하고 Debug clean configure/build 수행.
- `kcf_core`, `kcf_dummy`, `kcf_bringup` 생성 성공, 빌드 출력에 경고·오류 없음.
- Dummy 단독 실행에서 Setup 1회, 약 2 Hz Loop(498~503ms), Ctrl+C 후 Shutdown 1회 확인.
- Bringup의 child 생성과 Ctrl+C 종료 흐름, Bringup에만 전달한 SIGTERM의 child 전달 확인.
- child 선종료 감지와 재시작 없는 종료 확인.
- `waitpid()` 회수 및 중복 회수 방지 확인, 잔류 child·zombie 없음.
- 계층별 책임, 정상·Setup 실패 상태 전이, signal handler와 scheduler 구현 확인.
- Phase 1 범위 밖 기능 없음. 통합 검증 결과 PASS, 코드 수정 없음.

## v0.6 — Bringup / Supervisor

- `bringup::Bringup`의 `Setup()`, `Run()`, `Shutdown()` 구현.
- `kcf::Process`로 Dummy child 하나를 생성하고 외부 lifecycle 관리.
- `sigaction()`과 단순 stop flag로 SIGINT/SIGTERM 처리.
- 100ms 간격으로 stop 요청과 child 상태 확인, child 선종료 시 재시작 없이 종료.
- 종료 시 child에 SIGTERM 요청 후 `Wait()`로 회수하고 기존 signal handler 복구.
- 실행 파일 경로를 인자로 받는 `kcf_bringup` target 추가.
- 빌드, Ctrl+C, Bringup 단독 SIGTERM, child 선종료, 인자 누락 처리 및 zombie 부재 검증.

## v0.5 — Dummy Element와 실행 예제

- `ProcessElement`를 상속하는 `DummyElement` 추가.
- Setup·Loop·Shutdown에서 각각 지정된 로그만 출력하고 Setup은 0 반환.
- 얇은 `main.cpp`에서 ProcessRuntime을 생성하고 실행 주기를 2 Hz로 설정.
- `kcf_core`에 링크하는 `kcf_dummy` executable target 추가.
- configure/build 및 PTY 실행 검증 성공: Loop 약 500ms, Ctrl+C 후 Shutdown 1회, 종료 코드 0.

## v0.4 — Linux Process wrapper

- `kcf::Process`와 `Start()`, `RequestStop()`, `IsRunning()`, `Wait()`, `GetPid()` 추가.
- `fork()` 후 `execl()`로 실행 파일 직접 실행, exec 실패 시 `_exit(127)` 사용.
- 실행 중 중복 Start 차단, 종료된 객체의 재사용과 PID 관리 구현.
- `RequestStop()`의 SIGTERM 요청과 `Wait()`의 blocking 회수 책임 분리.
- `IsRunning()`에서 `waitpid(WNOHANG)`으로 회수한 status 저장, 이후 Wait의 중복 회수 방지.
- EINTR 재시도 및 단순 정수 오류 반환 적용. 소멸자는 child를 자동 종료하지 않음.
- `process.cpp`를 `kcf_core`에 추가하고 compile/link 성공 확인.

## v0.3 — ProcessRuntime

- 공통 `ProcessRuntime`과 `Run()`, `RequestStop()`, `IsRunning()`, `GetState()`, `SetLoopFrequency()` 구현.
- Setup → 주기적 Loop → Shutdown 실행 흐름과 lifecycle state 관리.
- Setup 실패 시 Shutdown 없이 Runtime 정리 후 ERROR 및 실패 코드 반환.
- SIGINT/SIGTERM handler는 `volatile sig_atomic_t` flag만 설정하고 정상 실행 문맥에서 종료 처리.
- Runtime 종료 시 기존 signal handler 복구.
- `steady_clock` / `sleep_until()` 기반 scheduler 구현, 기본 10 Hz 및 overrun 시 기준 시각 재설정.
- `kcf_core`를 INTERFACE에서 실제 compiled library로 변경하고 빌드 성공 확인.

## v0.2 — Lifecycle State와 Element 인터페이스

- `kcf::ProcessState`를 `std::uint8_t` 기반 enum class로 정의.
- 상태: `STOPPED`, `STARTING`, `RUNNING`, `STOPPING`, `ERROR`.
- `ProcessElement`에 기본 가상 소멸자, 순수 가상 `int Setup()`과 `void Loop()` 정의.
- 선택적으로 재정의할 수 있는 빈 `Shutdown()` 기본 구현 제공.
- 기존 빌드 성공 확인. 이 단계에서는 header 자체를 컴파일하는 target을 추가하지 않음.

## v0.1 — 프로젝트 디렉터리와 CMake 계층

- `kcf`, `examples/dummy`, `bringup`의 기본 include/src 디렉터리와 CMake 파일 생성.
- CMake 최소 3.16, 프로젝트명 `kss_control_framework`, C/CXX 및 C++17 설정.
- Threads package 검색 및 하위 영역의 명시적 `add_subdirectory()` 등록.
- `kcf_core`를 임시 INTERFACE library로 정의하고 include 경로와 `Threads::Threads` 연결.
- Dummy·Bringup은 CMake 위치만 준비하고 executable 및 placeholder source는 생성하지 않음.
- `.gitignore`에 `build/` 추가, Debug configure/build 성공 확인.
