# Changelog

KSS Control Framework(KCF)의 주요 개발 및 검증 이력입니다.
기존 Phase 1-1~1-7의 v0.1~v0.7 이력은 유지하고 Phase 1 완료 기준을 v1.0으로 기록합니다.
Phase 2 통신 기능은 v2.0, Phase 3 supervision/recovery는 v3.0에 기록합니다.
버전은 최신순으로, 각 버전 내부는 Phase 진행 순서대로 정리합니다.
이 문서의 버전 표기는 개발 이력 구분이며 Git tag 또는 배포 생성 여부를 의미하지 않습니다.

## v4.1 — Mecanum M-0 / Runtime Lifecycle Contract

### M-0 — Application Data Contract

- `applications/mecanum/data/`에 common/motor/imu/lidar 헤더 추가. Framework 공통 타입과 분리.
- Producer sequence·monotonic timestamp, 3축 VelocityCommand/OdometryData, IMU 단위·valid mask 정의.
- 제품 독립적인 FixedLidarScan container와 타입 특성·양수 capacity static_assert 추가.
- C++17 개별/통합 include, 기본값 및 잘못된 template 인자 거부 검증 PASS.
- 실제 Motor/IMU/LiDAR Element 및 Driver 구현은 포함하지 않음.

### Runtime Lifecycle / Error Contract

- **API 변경:** ProcessElement의 `void Loop()`를 `int Loop()`로 변경. 0은 정상 cycle, non-zero는 fatal runtime error.
- Setup 진입 후 성공·오류 반환·예외와 관계없이 Shutdown을 정확히 1회 호출. Setup 전 내부 초기화 실패는 호출하지 않음.
- Setup/Loop 예외는 -EFAULT로 처리하고, Shutdown 예외가 기존 오류를 덮어쓰지 않도록 유지.
- 실패한 Loop cycle은 heartbeat를 증가시키지 않고 ERROR → Shutdown → 원래 오류 반환으로 종료.
- 기존 Supervisor loss 우선순위와 SIGINT/SIGTERM/RequestStop·Reset 정상 종료 계약 유지.
- 기존 example/test Element의 signature와 정상 반환 경로 이식. Integration의 Setup 내부 직접 Shutdown을 제거하여 중복 정리 방지.
- `kcf_runtime_lifecycle_test` 추가: 정확한 Run 오류값·Runtime status, heartbeat, Shutdown 횟수 및 부분 자원 정리 검증.
- 기존 Element는 int Loop로 이식 후 재빌드 필요. Mecanum 데이터·통신 내부·Bringup 정책은 변경하지 않음.
- C++17 clean build와 기존 예제 빌드, lifecycle·Runtime supervision·20 Element/1 kHz·Reset 10회·Supervisor loss 회귀 PASS.
- Topic/Service/Parameter/Action 및 Timer 포함 Integration 10 lifecycle PASS. 부분 초기화 자원·FD·child·SHM 정리 확인.

## v4.0 — Mecanum 헤더 추가 전 기준

- v3.0 supervision/recovery 및 Phase 3-8/3-9A/3-9B hardening을 완료한 Framework 기준 상태.
- 이후 v4.1 M-0 데이터 계약 및 Runtime lifecycle 보완 작업의 기준. 세부 이력은 아래 v3.0의 Phase별 항목 유지.

## v3.0 — System Supervision / Fault Management

Phase 3-1~3-7 통합 검증과 Phase 3-8/3-9A/3-9B hardening을 완료했습니다. **v3.0 PASS**.

### Phase 3-1 — Multi-Element Supervisor Base

- Bringup을 여러 Element process를 관리하는 Supervisor로 확장.
- Element별 이름·실행 파일·인자를 관리하고 `--next`로 여러 실행 항목을 지정하는 CLI 추가.
- Process 인자 전달 및 ProcessExitInfo로 정상 exit code와 signal 종료 원인 조회 지원.
- Child 실패를 개별 기록하고 surviving Element는 유지. 사용자 종료 시 전체 stop/reap 수행.
- 다중 Element 실행, 부분 startup 실패 정리, 인자·종료 정보 및 zombie 부재 검증 PASS.

### Phase 3-2 — Application State / Error Latch

- 공통 ApplicationState의 INITIALIZING / RUNNING / ERROR / SHUTTING_DOWN 관리 추가.
- RUNNING 중 최초 unexpected failure의 Element 이름·PID·종료 원인을 ApplicationError에 보존.
- Secondary failure는 Element별로 기록하며 최초 origin을 덮어쓰지 않음.
- ERROR 자동 해제·자동 restart 없이 사용자 종료까지 surviving Element 유지.
- 상태 전이, 최초 origin 보존, secondary failure 및 ERROR 상태 종료 검증 PASS.

### Phase 3-3 — SystemStatus Broadcast / Element Safe-State Reaction

- Supervisor가 소유하는 SystemStatus Topic으로 Application 상태와 최초 오류를 반복 발행.
- SafeElement fixture가 ERROR를 받으면 output=0으로 전환하고 SAFE latch 유지.
- ERROR 이후에도 Element process와 Loop가 생존하는지 확인.
- SystemStatus sequence·최초 origin·SAFE 유지 및 종료 시 Topic 정리 검증 PASS.
- KCF software SAFE와 Device watchdog/hardware safety의 책임을 구분하여 문서화.

### Phase 3-4 — Crash IPC Recovery Base

- Topic/Parameter에 owner metadata와 dead owner 기반 stale SHM recovery 추가.
- Robust mutex의 owner death 복구 및 condition wait/종료 처리 보완.
- Parameter transaction 복구로 crash 중 부분 변경이 정상 값으로 노출되지 않도록 처리.
- Crash로 남은 reader pin 때문에 Action 상태 발행이 무한 재시도하던 문제를 EAGAIN 반환으로 수정.
- Topic/Parameter 각 20 generation recovery와 실제 Parameter Set 중 30회 crash 검증 PASS.
- 기존 generation 전체 종료 후 복구하는 정책을 유지하며 hot reconnect는 제공하지 않음.

### Phase 3-5 — Runtime Supervision Channel / Health Monitoring

- UNIX socketpair 기반 Runtime 상태 요청·응답과 PID/request_id 검증 추가.
- Runtime worker를 Setup 전에 시작하여 STARTING을 관찰하고, 전체 Runtime RUNNING 초기화 barrier 구현.
- Loop 정상 반환 시 Runtime이 heartbeat를 자동 증가. Standalone 실행에는 supervision worker 없음.
- PROCESS_EXIT / RUNTIME_ERROR / STATUS_TIMEOUT / HEARTBEAT_STALL을 구분하여 ERROR latch와 SystemStatus에 반영.
- Health fault 감지 후 자동 kill/restart 없이 ERROR/SAFE 유지.
- Startup 실패·timeout, Loop stall, SIGSTOP, Runtime ERROR 및 20 Element supervision 검증 PASS.

### Phase 3-6 — User Reset / Controlled Reinitialize

- 명시적 `RequestReset()`과 ERROR → RESETTING → RUNNING lifecycle 추가.
- 전체 old generation에 SIGTERM을 보내고 timeout 시 SIGKILL 후 모두 reap한 뒤 새 generation 시작.
- Reset 중 최초 오류와 SystemStatus Topic을 유지하고, 새 Runtime 전체 RUNNING 확인 후에만 오류 해제.
- Reset 실패는 partial generation을 정리하고 ERROR로 복귀. 다음 명시적 요청으로 재시도 가능.
- SafeElement를 initial output=0으로 변경하고 RUNNING 확인 후에만 정상 output 허용.
- 반복 Reset 10회, 실패 후 재Reset, stuck child 정리 및 Reset 중 사용자 종료 우선 처리 검증 PASS.

### Phase 3-7 — Fault / Recovery Integration Verification

- Fault → ERROR/SAFE → 명시적 Reset → 전체 generation 복구 흐름을 통합 검증.
- 새 client가 dead owner의 stale SHM에 연결되는 결함을 재현하고 Topic/Parameter Open에서 EAGAIN 반환하도록 수정.
- Open 재시도의 shared lock이 새 owner Create와 경합하지 않도록 dead owner를 lock 전에 확인.
- Topic·Parameter 동시 복구, 새 consumer 데이터 수신, 비운전 output=0 및 SAFE 자동 해제 부재 검증.
- 혼합 fault/recovery 10회, Reset 실패·재시도·종료 우선 처리, 20 Element 검증 PASS.
- v2.0 통신, crash recovery, Action EAGAIN 및 Integration 10 lifecycle 전체 회귀 PASS.
- 최종 clean build, FD·child·SHM 정리와 UDP port 재사용 확인. 공개 문서를 v3.0 완료 기준으로 갱신.

### Phase 3-8 — Supervisor Loss Detection / Supervised Element Shutdown

- 기존 RuntimeStatusRequest를 heartbeat로 사용하여 supervised Runtime만 Supervisor 생존 감시.
- Socket loss는 ECONNRESET, 유효 요청 2초 timeout은 ETIMEDOUT으로 기록하고 non-zero 종료.
- Worker는 atomic stop만 요청하고 main Runtime 경로가 Shutdown 수행. Setup 중 loss 후 Loop 진입 차단.
- Standalone 및 정상 SIGTERM/Reset 종료 유지. Fast Loop에 socket/timeout 검사나 추가 thread 없음.
- 실제 Supervisor SIGKILL/SIGSTOP 후 A/B/C 자체 종료·reap, invalid packet timeout 및 Setup-time loss 검증 PASS.
- Clean build, protocol·20 Element·Reset 10회·통신·crash recovery·Integration 10회 회귀 PASS.
- 무한 Setup/Loop의 강제 중단은 제공하지 않음. 외부 manager restart와 Device watchdog은 KCF 외부 책임.

### Phase 3-9A — Persistent SystemStatus Crash Hardening

- SystemStatus 전용 Publisher/Subscriber를 추가하고 기존 SharedParameter의 robust shared-state 동기화·복구 재사용.
- 일반 Topic의 triple buffer·reader pin 구현은 유지하면서 persistent SystemStatus의 crash-leaked pin 누적 경로 제거.
- Child 실행 전 INITIALIZING 생성, 초기 snapshot callback, Reset 동안 동일 inode 및 ERROR origin 유지.
- Bringup·SafeElement·SystemStatus probes를 전용 API로 변경. 새 저장소 `/kcf/system/status/state` 사용, legacy 자동 migration 없음.
- 동일 객체에서 mutex를 소유한 reader crash 20회, N readers·callback 재진입·ENOTRECOVERABLE·writer recovery 검증 PASS.
- Clean build, Reset 10회, Supervisor loss, 20 Element/1 kHz, 기존 통신·Topic/Parameter recovery·Integration 회귀 PASS.
- 이 단계에서는 일반 Topic/Parameter 구현을 유지. Incomplete SHM Open/Create 경합은 다음 Phase 3-9B에서 검증·수정.

### Phase 3-9B — Incomplete SHM Open/Create Recovery Race

- TEST-FIRST로 Topic·Parameter의 incomplete object Open/Create 경합 재현: shared flock 때문에 replacement Create가 -EEXIST 반환.
- 짧은 header에서 Open이 재시도 경로를 벗어나는 -EPROTO/-EMSGSIZE를 반환하는 문제도 확인.
- 짧은 header 및 initialized=0에 대한 Open을 flock 전 -EAGAIN으로 처리. Live initializer 보호와 post-lock 검증 유지.
- Production 변경은 두 Open의 initialization 경로에 한정. Create 및 정상 Topic/Parameter fast path 변경 없음.
- `kcf_incomplete_recovery_test` 추가. 크기 0·짧은 header·미완료 header·전체 크기 미초기화의 4 stage 검증.
- Production hook 없이 test-only flock 순서 제어 및 barrier 사용. Transport별 400회 동시 시작·4개 결정적 순서, 각각 404/404 PASS.
- Consumer retry 2초·전체 실행 60초 제한. Live owner·unknown complete header 보호, 새 inode·payload/version 및 stale attach 부재 확인.
- Reset 중 Topic/Parameter initializer SIGKILL 후 ERROR/origin 유지, 다음 명시적 Reset에서 전체 Runtime RUNNING 및 fresh 값 수신 PASS.
- Clean build, 기존 crash recovery·SystemStatus·Reset 10회·Supervisor loss·20 Element/1 kHz·통신·Integration 10 lifecycle 회귀 PASS.
- FD baseline 유지, child reap, 잔여 process·zombie·SHM 없음 및 UDP 포트 재사용 확인. **Phase 3-9B PASS**.

자동 restart·부분 restart·hot reconnect는 제공하지 않습니다.
KCF software SAFE는 Device watchdog/hardware safety를 대체하지 않으며,
Linux kernel uninterruptible sleep(D state)은 SIGKILL 이후에도 bounded 종료를 보장할 수 없습니다.

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
