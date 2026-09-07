# Changelog

KSS Control Framework(KCF)의 주요 개발 및 검증 이력입니다.
기존 Phase 1-1~1-7의 v0.1~v0.7 이력은 유지하고 Phase 1 완료 기준을 v1.0으로 기록합니다.
Phase 2-1~2-5 완료 내용은 v2.0으로 묶으며, 최신 버전부터 기록합니다.
이 문서의 버전 표기는 개발 이력 구분이며 Git tag 또는 배포 생성 여부를 의미하지 않습니다.

## v2.0 — Topic / Timer / Service / Runtime Parameter / Action

v1.0 Process Framework를 유지하면서 Phase 2-1~2-5의 통신 및 실행 보조 기능을 추가했습니다.

### Topic — Phase 2-1

- `SharedChannel<T>`, `Publisher<T>`, `Subscriber<T>` 추가.
- POSIX Shared Memory 기반 1 Publisher : N Subscriber latest-value 통신 구현.
- Atomic triple buffer와 reader pin으로 snapshot 복사 중 데이터 경합 방지.
- Payload와 분리된 process-shared condition variable 알림 및 Subscriber worker callback 구현.
- 느린 수신자의 중간 값 생략, 초기화 경합 처리, 명시적 Close/Unlink 지원.
- `kcf_topic_pub`, `kcf_topic_sub` 예제와 다중 프로세스 snapshot·종료 검증 완료.

### Timer — Phase 2-2

- `Timer::Create()`, `Stop()`, `IsRunning()`, `GetFrequency()` 추가.
- ProcessRuntime과 독립적인 worker에서 steady clock 기반 주기 callback 실행.
- callback은 내부 mutex 밖에서 실행하며 Stop 시 대기 해제와 worker join 수행.
- `kcf_timer_test` 추가. 2 Hz / 100 Hz, overrun, Stop 및 Topic 연동 검증 완료.

### Service — Phase 2-3

- `ServiceServer`, `ServiceClient`와 typed Request / Response 구현.
- 프로세스당 하나의 localhost UDP 서버 port에서 여러 service ID 처리.
- 동기식 호출, timeout/retry, 요청·응답 ID 및 payload 검증 추가.
- 동일 요청 재시도는 동일 transaction ID를 유지하며 최근 64개 응답 캐시로 중복 callback 방지.
- `kcf_service_server`, `kcf_service_client` 추가.
- 연속 1,000회 호출, 중복·잘못된 요청, port 충돌, timeout, 지연 서버 시작 및 응답 검증 PASS.
- 중복 보호는 캐시 보존 범위에 한정되며 timeout이 미실행을 의미하지 않음.

### Runtime Parameter — Phase 2-4

- `Parameter<T>`와 Shared Memory 기반 현재 설정값 관리 구현.
- Create/Open, Get/Set, 값과 uint64 version의 일관된 snapshot 제공.
- process-shared mutex/condition variable로 여러 프로세스의 변경 동기화.
- 선택적 watcher worker에서 내부 lock 없이 callback 실행. 느린 watcher는 최신값 우선 처리.
- `kcf_parameter_owner`, `kcf_parameter_client` 추가.
- 다중 프로세스 Set 4,000회, callback에서 Get 재호출, lifecycle 100회 및 초기화 경합 검증 PASS.
- 디스크 영속 저장과 startup config loader는 포함하지 않음.

### Action — Phase 2-5

- Header-only `ActionServer<Goal, Feedback, Result>` 및 `ActionClient` 추가.
- 공통 상태 IDLE / ACCEPTED / RUNNING / SUCCEEDED / FAILED / CANCELED 정의.
- 기존 ServiceServer에 Goal/Cancel/GetResult ID를 등록하고 기존 Topic으로 상태·Feedback 발행.
- Action 전용 서버 socket이나 작업 worker 없이 application FSM에서 실제 작업 수행.
- cancel_requested와 최종 CANCELED 분리, 한 active Goal 제한, terminal 이후 다음 Goal 허용.
- Result는 다음 Goal 수락 전까지 보존하고 Service로 조회. 느린 Feedback 수신과 독립적으로 동작.
- Service 캐시 외에 현재/직전 Goal ID를 확인하여 늦은 중복 Goal의 재실행 방지.
- 내부 mutex 밖에서 callback과 Publish 실행, 동시 변경의 최신 상태 발행 순서 관리.
- `kcf_action_server`, `kcf_action_client` Count 예제 추가.
- 성공·실패·취소, Busy/잘못된 ID, 중복 요청, 느린 Feedback, 연속 Goal 검증 PASS.
- 동시 Feedback 2,000회 및 lifecycle 25회에서 교착·FD/SHM 누수 없음.

### 통합 검증 및 문서

- 기존 target과 Phase 2 예제를 포함한 Debug clean configure/build 성공.
- Phase 2-5 완료 후 Topic / Timer / Service / Parameter 회귀 검증 PASS.
- Readme를 v2.0 기능, 구조, 빌드·실행 방법 및 검증 결과로 갱신.
- 로컬 개발 문서 `docs/`와 `build/`의 Git ignore 유지.

## v1.0 — Phase 1 완료 기준

- v0.1~v0.7에서 개발·검증한 Process Execution Framework의 완료 기준.
- ProcessElement / ProcessRuntime / Process / Dummy / Bringup 포함.
- 정상 lifecycle, 주기 실행, signal 종료 및 child 회수 통합 검증 완료.
- 이후 Phase 2 개발의 기준 상태로 사용. 세부 개발 이력은 아래 v0.1~v0.7 항목 유지.

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
