# KSS Control Framework (KCF) Architecture Plan

## 1. Project Purpose

KSS Control Framework(KCF)는 Linux 기반 제어 시스템에서 반복적으로 사용되는
프로세스 실행 구조, lifecycle, IPC, command/state 구조를 공통화하기 위한
재사용 가능한 제어 Application Framework이다.

KCF는 특정 매카넘 로봇 전용 Framework가 아니다.

향후 다음과 같은 시스템에 공통적으로 사용하는 것을 목표로 한다.

- Mobile Robot
- Robot Arm
- Industrial Equipment
- Sensor / Actuator Control System
- SBC 기반 제어 Application
- 기타 Linux 기반 제어 시스템

매카넘 로봇은 KCF의 첫 Reference Application으로 사용한다.

KCF의 목표는 ROS를 다시 구현하는 것이 아니다.
Linux/POSIX API의 반복적인 사용 부분을 얇게 공통화하되,
프로세스와 통신 구조가 코드에서 명확하게 보이도록 한다.


## 2. Overall Architecture

전체 시스템은 Frontend와 Backend를 분리한다.

Frontend
    ↕ Communication
Backend
    ├ Bringup / Supervisor
    └ Element Processes
        └ ProcessRuntime
            └ Setup / Loop / Shutdown


## 3. Frontend / Backend Principle

Backend는 다음을 소유한다.

- Device Control
- Application Logic
- State
- FSM
- Safety Logic
- Algorithm
- Sensor Processing

Frontend는 다음만 담당한다.

- 상태 표시
- 사용자 입력
- 데이터 송신
- 데이터 수신

UI 내부에는 실제 제어 로직을 넣지 않는다.

UI가 종료되어도 Backend는 독립적으로 정상 동작해야 한다.

Frontend 구현은 Backend와 독립적이어야 한다.

예:

- Qt
- Flutter
- Web UI
- CLI Test Client


## 4. Backend Hierarchy

Backend의 기본 계층은 다음과 같다.

Bringup
    ↓
Element Process
    ↓
ProcessRuntime
    ↓
ProcessElement
    ↓
Component

각 계층의 책임은 명확하게 분리한다.


## 5. Bringup / Supervisor

Bringup은 Application 전체의 프로세스를 관리하는 Supervisor이다.

책임:

- Child Process 생성
- PID 관리
- Process 실행 상태 확인
- 정상/비정상 종료 확인
- Shutdown 요청
- Child Process 종료 대기

Bringup은 Element 내부의 Driver나 Algorithm을 직접 알지 않는다.

예:

Bringup
 ├ Motor Process
 ├ IMU Process
 ├ LiDAR Process
 └ EKF Process


## 6. Element Process

Element는 하나의 주요 기능을 담당하는 독립 Process이다.

예:

- MotorElement
- ImuElement
- LidarElement
- EkfElement
- CameraElement
- AxisElement

Element는 내부에서 필요한 Component를 생성하고 관리한다.

예:

MotorElement
 ├ MotorDriver
 ├ MecanumKinematics
 ├ Odometry
 └ IPC Interface

Bringup은 MotorDriver를 직접 관리하지 않는다.


## 7. ProcessRuntime

모든 Element Process는 공통 ProcessRuntime을 사용한다.

ProcessRuntime이 담당하는 기능:

- Lifecycle 관리
- Setup 실행
- Loop 실행
- Shutdown 실행
- SIGINT / SIGTERM 처리
- Loop Scheduler
- Stop Request 관리
- 향후 callback dispatch

Framework 사용자는 기본적으로 다음 함수만 구현한다.

int Setup();
void Loop();
void Shutdown();

Setup과 Loop는 필수이다.

Shutdown은 필요한 Element만 구현할 수 있도록
기본 empty implementation을 제공할 수 있다.

Element의 main.cpp는 가능한 한 얇게 유지한다.

예:

int main()
{
    MyElement element;
    kcf::ProcessRuntime runtime;

    runtime.SetLoopFrequency(100.0);

    return runtime.Run(element);
}


## 8. Process Lifecycle

공통 Process State는 초기에는 다음 상태만 사용한다.

STOPPED
STARTING
RUNNING
STOPPING
ERROR

정상 흐름:

STOPPED
 → STARTING
 → Setup()
 → RUNNING
 → Stop Request
 → STOPPING
 → Shutdown()
 → STOPPED

Setup 실패 또는 치명적인 실행 오류가 발생하면 ERROR 상태로 전환한다.

초기 구현에서 불필요한 상태를 미리 추가하지 않는다.


## 9. Communication Model

KCF는 ROS의 사용 경험에서 유용했던 통신 의미를 참고하지만
ROS Middleware 자체를 복제하지 않는다.


### 9.1 Topic

Phase 2-1 Topic은 POSIX Shared Memory 기반 latest-value snapshot 통신이다.

- 1 Publisher : N Subscriber
- 센서값, 상태값, 연속 제어 명령의 최신 Published Snapshot을 우선한다.
- 느린 Subscriber는 중간 Publish를 건너뛸 수 있다. 과거 Message Queue나 순차 전달을 보장하지 않는다.
- callback에 채택한 Snapshot의 무결성을 보장하며 torn/incomplete snapshot은 전달하지 않는다.
- 의미 있는 단발 Event / Command / Transaction은 이후 Service / Action에서 유실·중복·결과를 별도로 관리한다.

사용자 API는 T의 종류·크기와 관계없이 `kcf::Publisher<T>` / `kcf::Subscriber<T>`이다.
Element의 Setup에서 Create로 등록하고, 실행 중 Publisher::Publish로 snapshot을 발행한다.
Publisher에 timer/thread/scheduler를 넣지 않는다. 주기 관리는 이후 Timer의 책임이다.

```text
Publisher<T> → SharedChannel<T> → POSIX Shared Memory
                                  ├ Atomic Triple Buffer
                                  ├ Snapshot Metadata
                                  └ Wait / Notify
Subscriber<T> → worker Wait → ReadLatestSnapshot → callback(local_snapshot)
```

Data path:

- 3개 Slot 각각에 version, atomic reader/writer state, 해당 payload의 publish sequence와 T 저장 공간을 둔다.
- Channel에는 atomic published_index / publish_sequence 및 initialization metadata를 둔다.
- 공유 atomic metadata는 lock-free 32-bit atomic이며 compile-time에 검증한다.
- 현재 Published Slot을 직접 덮어쓰지 않고 다음 Slot을 순환 선택한다.
- Writer: slot 확보 → version 홀수 → T copy → version 짝수 → published_index 공개 → publish_sequence 갱신.
- Reader: index 확인 → slot의 짧은 atomic reader pin → stable version_before 확인 → local T copy → version_after 확인 → pin 해제.
- version_before == version_after이고 stable인 경우만 채택한다. 충돌 시 최신 index부터 다시 읽는다.
- reader pin은 비원자 memcpy와 writer 재사용이 겹치는 C++ data race를 막는다. version 재검사만으로 비원자 payload의 동시 접근을 정당화하지 않는다.
- 두 non-published Slot이 모두 사용 중이면 Publish는 -EAGAIN을 반환하며 caller가 재시도할 수 있다. Reader를 기다리며 무한 정지하지 않는다.
- Payload copy, version 검증, published_index 접근에는 pthread mutex를 사용하지 않는다.
- T는 trivially copyable이어야 하며 pointer/heap-owned member를 포함하지 않는다. 같은 T/layout/ABI를 공유해야 한다.

Notification path (payload protection과 분리):

- process-shared pthread_mutex_t / pthread_cond_t와 notify_sequence를 둔다.
- Creator는 mutex/cond attribute에 PTHREAD_PROCESS_SHARED를 설정한다.
- Publisher는 snapshot 공개 완료 후 notify_mutex 안에서 notify_sequence만 갱신하고 unlock 후 broadcast한다.
- Subscriber는 notify_mutex 안에서 while predicate로 새 notify_sequence 또는 local stop을 기다린다.
- Wait가 mutex를 해제한 뒤 Triple Buffer snapshot을 읽는다. notification critical section에는 payload copy가 없다.
- Subscriber는 마지막 callback의 publish sequence를 기억하여 같은 publish를 중복 전달하지 않는다.
- slot의 sequence와 payload를 함께 읽어 index 전환 중 다른 publish의 sequence가 결합되지 않도록 한다.
- 32-bit sequence는 순환한다. 두 관찰 사이에 정확히 2^32번의 Publish가 일어나는 구분은 보장하지 않는다.

Callback과 종료:

- Phase 2-1 callback은 Subscriber 내부 worker thread에서 직접 실행되어 ProcessElement::Loop와 다른 context일 수 있다.
- callback은 local snapshot만 받으며 notification mutex, reader pin을 포함한 모든 KCF 내부 동기화를 해제한 상태에서 실행된다.
- 사용자 공유 상태의 동기화는 사용자가 담당한다. callback은 반환해야 Close의 join이 완료된다.
- Close/destructor는 controlling thread에서 실행한다. callback 내부에서 Subscriber를 파괴하지 않는다.
- Close: local running false → notify predicate와 같은 mutex 아래 stop/broadcast → worker join → unmap/close.
- 종료 broadcast로 깨어난 다른 Subscriber는 predicate를 재검사한다. 수신 대기는 busy polling하지 않는다.

Shared Memory lifecycle:

- Create: shm_open(O_EXCL) → initialization flock → ftruncate/mmap → atomic/pthread 초기화 → initialized → flock 해제.
- Open도 initialization flock 아래 크기·magic·format·initialized를 확인한 뒤에만 공유 primitive를 사용한다.
- 초기 크기가 0이거나 초기화 미완료이면 -EAGAIN을 반환한다. 없는 channel은 -ENOENT이다.
- Close는 munmap/close, Unlink는 owner의 명시적 shm_unlink이다. destructor에서 자동 unlink하지 않는다.
- Unlink 후 기존 Subscriber mapping은 Close까지 유효하다. 연결된 reader가 있을 수 있으므로 공유 pthread 객체를 destroy하지 않는다.
- `/kcf_test_topic`은 그대로 사용한다. 내부 slash와 %를 가진 논리 이름은 충돌 없이 인코딩한다.
- 정상 종료를 대상으로 한다. 복사/알림 동기화 도중 process 강제 종료에 대한 자동 복구는 이번 범위 밖이다.

Phase 2-1 예제는 kcf_topic_pub / kcf_topic_sub이며 /kcf_test_topic을 사용한다.
Publisher 인자 [hz: 1..1000], Subscriber 인자 [callback_delay_ms: 0..5000] [log_every: >0]로
기본 수신, 느린 callback, 짧은 1 kHz 검증을 수행할 수 있다.
Timer, Service, Action, Parameter, callback executor, QoS, queue, discovery,
serialization, futex 및 Windows backend는 구현하지 않는다.


### 9.2 Service (Phase 2-3)

Service는 짧은 단발 Event / Command의 synchronous Request → Response이다.
Topic은 최신 상태용이며 Homing, Move, Calibration, Docking 같은 긴 작업은 이후 Action으로 처리한다.

Endpoint / API:

- localhost UDP (AF_INET/SOCK_DGRAM), Server는 127.0.0.1에만 bind한다.
- Element Process당 ServiceServer와 UDP port 하나를 사용한다. 여러 service_id를 같은 port에서 multiplex한다.
- Setup: Create(port) → Register<Request, Response>(id, callback) 반복 → Start().
- 중복 등록은 -EEXIST, worker 시작 후 등록은 -EBUSY이다.
- Port는 향후 Bringup이 runtime에 할당/전달한다. 이번 예제는 command-line port를 사용한다.
- Client Create(target_port, timeout_ms=200, retry_count=2)는 loopback ephemeral source port를 사용한다.
- 같은 ServiceClient의 Call/Create/Close는 mutex로 직렬화한다. 한 instance의 outstanding request는 하나이다.

Protocol / validation:

- 동일 Application ABI/layout의 native-endian local IPC이다. 범용 serializer나 외부 wire protocol이 아니다.
- 40-byte header: magic, protocol_version, REQUEST/RESPONSE, service_id, client_id, request_id, payload_size, framework_status.
- typed Request/Response는 trivially copyable이며 최대 4096 bytes이다. pointer/heap ownership은 포함하지 않는다.
- Framework 결과는 header의 0/음수 오류, application 결과는 typed Response에 기록한다.
- unknown service는 -ENOENT, payload size 불일치는 -EMSGSIZE 응답으로 callback을 차단한다.
- Client는 sender 주소/port, magic/version/type, ID, 실제 길이 및 expected payload 크기를 검증한다.

Retry / duplicate:

- Create마다 getrandom 기반 64-bit client_id를 생성하고 Call마다 request_id를 증가시킨다.
- retry는 동일 socket/source endpoint, client_id/request_id/service_id/payload를 유지한다.
- retry_count=2는 최초 1회 + 재전송 최대 2회이다.
- poll과 steady_clock deadline으로 각 attempt를 제한한다. 잘못된 Response가 deadline을 연장하지 않는다.
- 모든 attempt timeout 시 -ETIMEDOUT. timeout이 Command 미실행을 의미하지는 않는다.
- Server는 최근 64개 Request/Response를 FIFO ring에 보관한다. 키는 client_id/request_id/service_id와 source endpoint이다.
- 같은 요청은 callback 대신 cached Response를 재전송한다. 같은 키의 payload 변경은 -EINVAL이다.
- 캐시는 callback 이후 전송 전에 저장한다. callback 예외도 -EFAULT 응답으로 캐시한다.
- 중복 실행 방지는 캐시 보존 범위 내에서만 보장한다. 퇴출/서버 재시작 이후 exactly-once를 보장하지 않으며 application의 idempotency가 필요할 수 있다.

Threading / lifecycle:

- 단일 Server worker가 poll → recvfrom → validation → duplicate 검사 → dispatch → cache → sendto를 순차 수행한다.
- callback은 local typed object로 호출하며 KCF internal mutex를 보유하지 않는다.
- Service callback과 Element::Loop / Timer / Topic callback은 다른 context일 수 있다. application 공유 상태는 사용자가 동기화한다.
- Server lifecycle API는 controlling thread에서 직렬화한다. callback은 짧게 반환해야 하며 server를 파괴하지 않는다.
- Stop: running false → 최대 약 50ms poll 대기/현재 callback 완료 → join → socket close. destructor도 Stop한다.
- callback 내부 Stop은 요청만 하고 이후 외부 Stop/destructor가 join한다.
- SO_REUSEADDR/PORT 없이 bind하여 같은 port의 두 번째 Server는 실패한다.
- UDP 자체의 전역 ordering을 보장하지 않는다. 동기식 Client 흐름과 Server 순차 callback으로 실행 순서를 유지한다.

예제: kcf_service_server <port>, kcf_service_client <port> [sequential_calls].
한 port에서 Add(1), SetValue(2)를 제공한다.
Bringup port 전달, async Service, Action, Parameter, discovery, registry, thread pool,
TCP/TLS, remote gateway는 이번 Phase에서 구현하지 않는다.


### 9.3 Action (Phase 2-5)

Action은 Goal / Feedback / Result / Cancel로 관리하는 장시간 application FSM이다.
공통 상태는 IDLE, ACCEPTED, RUNNING, SUCCEEDED, FAILED, CANCELED만 사용한다.
Goal reject는 응답 코드로 표현하며 별도 REJECTED/CANCELING 등의 상태를 추가하지 않는다.

Transport / 타입:

- ActionServer<Goal, Feedback, Result>는 기존 Process ServiceServer에 명시적 Goal/Cancel/GetResult ID 3개를 등록한다.
- Process당 UDP endpoint 하나를 유지한다. Action 전용 socket/port/protocol은 만들지 않는다.
- State/Feedback은 기존 Publisher<ActionStatus<Feedback>> Topic 하나로 발행한다.
- Goal/Feedback/Result는 application별 trivially-copyable POD이다. pointer/heap-owned member는 포함하지 않는다.
- Goal/Result envelope는 기존 Service payload 제한(4096 bytes)을 따른다.
- ActionHeader는 goal_id, feedback_sequence, state, uint8 cancel_requested와 명시적 reserved bytes로 24 bytes이다.
- Goal/Cancel response는 goal_id, result_code, accepted 및 reserved bytes로 16 bytes이다.
- Result response는 goal_id/state/ready/result_code와 typed Result를 포함한다.

Lifecycle:

- 한 ActionServer에 active goal 하나만 허용한다. ACCEPTED/RUNNING에서 다른 Goal은 -EBUSY이다.
- Goal callback은 내부 mutex 밖에서 실행하고 0이면 ACCEPTED, non-zero면 기존 상태를 유지한다.
- Goal callback은 작업 준비만 수행한다. ACCEPTED 확인 후 Element::Loop 또는 Timer가 Start(goal_id)로 RUNNING을 만든다.
- PublishFeedback은 current id의 RUNNING에서만 가능하고 feedback_sequence를 증가시킨다.
- Succeed/Fail은 RUNNING에서 Result를 저장하고 SUCCEEDED/FAILED로 전환한다.
- Cancel은 ACCEPTED/RUNNING에서 cancel_requested만 설정한다. application cancel callback은 중복 호출하지 않는다.
- application이 안전한 취소를 완료하고 Canceled를 호출해야 CANCELED가 된다. ACCEPTED 상태에서도 취소 완료 가능하다.
- invalid id/transition은 오류이며 현재 상태를 변경하지 않는다.
- terminal 이후 새 Goal을 수락할 수 있다. 이전 Result는 새 Goal 수락 전까지 보존한다.
- 현재/직전 수락 goal_id와 최근 Goal 결정 하나를 기억하여 Service 캐시 퇴출 후에도 동일 Goal을 재실행하지 않는다.
- 더 오래된 Goal history와 서버 재시작 이후 중복 방지는 보장하지 않는다.

Result / Client:

- ActionClient는 기존 ServiceClient와 Subscriber를 사용한다. Goal ID는 instance별 임의 64-bit 시작값에서 증가하며 재생성해도 같은 instance에서 재사용하지 않는다.
- SendGoal은 accepted 응답에서만 출력 goal_id를 갱신한다. Service retry는 동일 request_id와 동일 goal envelope를 유지한다.
- timeout은 Goal 거부/미실행을 의미하지 않는다. Goal id와 Service transaction id는 별개이다.
- Cancel 성공은 최종 취소 완료가 아니다.
- GetResult는 Service로 저장된 Result를 가져오며 진행 중 -EINPROGRESS, 모르는 id -ENOENT이다.
- 최종 Result는 Feedback delivery에 의존하지 않는다. 느린 Subscriber는 중간 상태/Feedback을 건너뛸 수 있다.
- feedback callback은 기존 Subscriber worker에서 실행된다. 새로운 executor는 없다.

Thread safety / 수명:

- Service worker와 application Loop/Timer 간 공통 상태는 짧은 mutex로 보호한다.
- immutable Goal/Feedback/Result snapshot 참조를 사용하여 큰 payload copy는 state mutex 밖에서 수행한다.
- goal/cancel callback 및 Publisher.Publish 동안 Action 내부 mutex를 보유하지 않는다.
- 발행 담당 caller 하나가 revision을 확인하며 최신 상태를 발행한다. 동시 변경이 이전 snapshot으로 덮이지 않으며 별도 worker를 만들지 않는다.
- Phase 3-4부터 Topic -EAGAIN은 Action caller에 반환한다. crash-leaked pin에서 무한 재시도로 종료를 막지 않으며 상태/Result commit은 유지된다.
- API의 상태 변경은 Topic 전송 전에 commit된다. 전송 오류가 나도 Result 조회는 Service에서 가능하다.
- ActionServer에 application 실행 worker는 없다. 실제 Count, motion 등의 작업은 application FSM이 수행한다.
- 종료 순서: application Loop/Timer 종료 → ServiceServer Stop/join → ActionServer Unlink → Close.
- Unlink는 명시적이며 destructor는 global SHM을 삭제하지 않는다.
- Service handler는 weak reference로 Action 상태에 접근한다. Close 후 새 요청은 -ESHUTDOWN이며 dangling this를 호출하지 않는다.
- 기존 ServiceServer에는 unregister가 없으므로 부분 등록 실패 후에는 ServiceServer를 Create하여 다시 등록한다.
- ActionClient Create/Close/destructor는 controlling thread에서 처리한다. feedback callback에서 자기 client를 파괴하지 않는다.
- 사용자 공유 상태는 context별 동기화가 필요하다. callback은 짧게 반환해야 한다.

예제는 CountGoal/CountFeedback/CountResult, Service ID 100/101/102,
/kcf_test_count_action/status Topic과 기본 port 22010을 사용한다.
kcf_action_server [port], kcf_action_client [port] [target_count] [callback_delay_ms] [cancel_after_ms].
Count 작업은 application main loop에서 100ms 주기로 수행한다.
동시 다중 goal, preemption, pause/resume, Action executor, persistent Result DB,
discovery, remote gateway, ROS compatibility, UI/config 통합은 구현하지 않는다.


### 9.4 Parameter (Phase 2-4)

Parameter는 실행 중 유지되는 current configuration state이다. Topic의 연속 데이터,
Service의 단발 명령, Action의 긴 작업과 구분한다. 현재 값은 항상 Get 가능하며 과거 변경 Queue는 없다.
여기서 persistent는 shared memory 수명 동안 현재 설정이 유지됨을 뜻하며 디스크 영속 저장을 뜻하지 않는다.

Startup Parameter는 향후 config file에서 시작 시 읽는다. Phase 2-4는 Runtime Parameter만 구현하며
YAML/JSON/INI parser, startup config loader, manifest 연동은 추가하지 않는다.

API와 구조:

- Parameter<T> → SharedParameter<T> → POSIX shared memory.
- Owner: Create(name, initial_value, optional_callback).
- Client: Open(name, optional_callback).
- Owner/Client 모두 Get(value), Set(value), Close() 사용 가능. Owner만 명시적 Unlink() 가능.
- Get(value, &version)으로 같은 mutex 안에서 값과 version을 함께 조회할 수 있다.
- T는 scalar 또는 fixed POD, trivially copyable이며 string/vector/pointer/heap ownership/virtual member를 포함하지 않는다.
- 같은 payload 크기·alignment·ABI/layout이 필요하다. 크기 불일치는 metadata로 -EMSGSIZE 검출하며 같은 크기의 다른 타입 식별은 하지 않는다.

Synchronization:

- Shared memory에 magic/format/initialized/payload size/alignment, process-shared mutex/cond, uint64_t version과 value를 둔다.
- mutex/cond attribute는 PTHREAD_PROCESS_SHARED이다. Topic Triple Buffer나 Topic header에 의존하지 않는다.
- Get은 mutex 아래 snapshot copy한다. Set은 mutex 아래 value copy + version 증가 후 unlock → cond broadcast한다.
- version 초기값은 0이다. 같은 값을 Set해도 증가하며 UINT64_MAX에서는 -EOVERFLOW로 변경을 거부한다.
- 여러 writer의 Set은 직렬화되며 각 value/version update는 완전하게 적용된다.
- Create/Open은 initialization flock으로 크기·metadata·pthread 초기화 경합을 막는다. initialized는 마지막에 기록한다.
- 미생성 name은 -ENOENT, 생성 중 zero-size/미초기화는 -EAGAIN이다.
- POSIX name의 내부 slash 및 %는 충돌 없는 인코딩으로 변환한다. 단순 /kcf_test_parameter는 그대로 사용한다.

Optional watcher:

- callback이 있으면 Parameter당 worker 하나를 시작한다. 없으면 worker를 생성하지 않는다.
- Watch 등록 시 현재 version을 기준으로 잡는다. 초기/current snapshot은 Get으로 조회하며 synthetic initial callback은 없다.
- while version == last_version && !stop predicate로 cond wait한다.
- 변경 시 local value/version copy → mutex unlock → callback(local_snapshot).
- local Set과 외부 Set 모두 동일한 version/broadcast/worker 경로로 전달한다. Set에서 callback을 직접 호출하지 않는다.
- slow watcher는 중간 version을 건너뛰고 최신값을 처리한다. callback 실행 시간 동안 Set은 callback을 기다리지 않는다.
- callback은 KCF internal lock 없이 Parameter worker에서 실행된다.
- ProcessElement::Loop, Subscriber callback, Timer callback, Service callback, Parameter callback은 다른 execution context일 수 있다. 공유 application state 동기화는 사용자 책임이다.

Lifecycle:

- Create/Open/Close/Unlink는 controlling thread에서 직렬화한다. Close 전 application Get/Set 작업을 끝낸다.
- Close: local running false → 같은 shared mutex 아래 stop/broadcast → worker join → munmap/close.
- 다른 watcher는 종료 broadcast를 spurious wakeup으로 처리한다. busy polling하지 않는다.
- 실행 중 callback은 반환해야 join이 완료된다. callback에서 자신의 Parameter를 파괴하지 않으며 Close는 -EDEADLK이다.
- callback 예외는 worker를 종료시키며 Close에 -ECANCELED로 보고한다.
- Close와 Unlink는 분리한다. destructor는 Close만 하며 global shared resource를 자동 삭제하지 않는다.
- Unlink 후 기존 mapping은 Close까지 유효하다. 다른 process의 사용 가능성이 있으므로 공유 pthread 객체를 destroy하지 않는다.
- 정상 lifecycle을 대상으로 하며 mutex 보유 중 process 강제 종료의 복구 기능은 추가하지 않는다.

예제: kcf_parameter_owner [callback_delay_ms], kcf_parameter_client [watch].
ControlParameter(kp/ki/kd/mode)를 /kcf_test_parameter에서 공유한다.
Registry/Manager, permission system, remote protocol, Service 기반 access, UI,
Action, unified executor, dynamic type/string/vector Parameter는 구현하지 않는다.


## 10. Common Message Types

Message 구조는 가능한 범위에서 ROS message semantics를 참고한다.

단, Shared Memory에서 안전하게 사용할 수 있도록
fixed-size / POD 중심 구조를 사용한다.

원칙:

- fixed-width integer
- fixed-size array
- std::string 사용 금지
- std::vector 사용 금지
- raw pointer 포함 금지
- owned heap object 포함 금지
- virtual member 포함 금지
- trivially copyable 구조 권장

초기 공통 타입:

- Time
- Header
- Vector3
- Point
- Quaternion
- Pose
- Twist
- PoseWithCovariance
- TwistWithCovariance
- Imu
- Odometry
- LaserScan
- Image
- CameraInfo
- RegionOfInterest

필요한 Custom Message / Service / Action 타입은
각 기능에서 별도로 추가한다.


## 11. Framework Structure Principle

KCF는 반복되는 OS API를 얇게 wrapping한다.

예:

- ProcessRuntime
- Process
- SharedChannel<T>
- UdpSocket
- UdpService
- SharedParameter<T>
- Action common state

하지만 다음과 같은 대형 Middleware 기능을 만들지 않는다.

- automatic discovery
- ROS-like node graph
- generic QoS system
- universal serialization framework
- hidden process manager
- 과도한 plugin abstraction

Process와 IPC 구조는 코드에서 추적 가능해야 한다.


## 12. Initial Reference Application

첫 실제 적용 대상은 Mecanum Robot이다.

초기 Element:

- Motor
- IMU
- LiDAR
- EKF

초기 데이터 흐름:

Twist
 → Motor Element

Motor Element
 → Wheel Odometry

IMU Element
 → Imu

LiDAR Element
 → LaserScan

Wheel Odometry + Imu
 → EKF Element
 → Filtered Odometry


## 13. Phase 1

Phase 1의 목적은 실제 장치 제어가 아니라
KCF의 Process Execution Framework를 완성하고 검증하는 것이다.

구현 대상:

1. Project / CMake structure
2. ProcessElement
3. Process lifecycle
4. ProcessRuntime
5. Linux Process wrapper
6. Dummy Element
7. Bringup / Supervisor
8. Integrated execution test

Phase 1에서는 다음을 구현하지 않는다.

- Topic
- SharedChannel
- Service
- Action transport
- Parameter
- Motor
- IMU
- LiDAR
- EKF
- 실제 Device Driver


## 14. Phase 1 Completion Condition

Bringup을 실행하면 Dummy child process가 생성되어야 한다.

예상 흐름:

kcf_bringup
 → spawn kcf_dummy
 → ProcessRuntime
 → STARTING
 → DummyElement::Setup()
 → RUNNING
 → periodic DummyElement::Loop()

사용자가 Ctrl+C를 입력하면:

Bringup
 → child SIGTERM

Dummy Process
 → STOPPING
 → DummyElement::Shutdown()
 → STOPPED
 → process exit

Bringup
 → waitpid
 → child 종료 확인
 → 정상 종료

이 흐름이 실제 Linux 환경에서 검증되면 Phase 1의 Process Runtime 기반이
완성된 것으로 본다.


## 15. Build Structure Direction

Top-level CMake에서 하위 영역을 명시적으로 등록한다.

예:

add_subdirectory(kcf)
add_subdirectory(examples/dummy)
add_subdirectory(bringup)

자동 recursive CMake discovery는 사용하지 않는다.

각 시스템 구성 요소가 top-level CMakeLists.txt에서
명확하게 보이도록 한다.

초기 Target:

- kcf_core
- kcf_dummy
- kcf_bringup


## 16. Timer (Phase 2-2)

Timer는 언제 실행할 것인가를 담당하는 periodic callback primitive이다.
Publisher는 데이터 전송만 담당하고 Timer와 독립적으로 유지한다.
Element의 Setup에서 `timer.Create(frequency_hz, callback)` 한 번으로 worker를 시작한다.
Loop 안에서 Update/Poll/Tick을 호출하지 않는다. 첫 callback은 한 주기 후 실행된다.


## 17. Element Independence
모든 Element는 자체 main()과 독립 executable CMake target을 가지며, Bringup 없이 단독 build/run/test가 가능해야 한다. Bringup은 동일 executable의 spawn, argument 전달, lifecycle 관리만 담당하며 Element 기능 자체를 제공하지 않는다.

API:

- `Create(double frequency_hz, std::function<void()> callback)`: 등록과 실행 시작, 성공 0 / 실패 음수 오류 코드.
- `Stop()`: stop predicate 설정, condition variable wake, worker join. 반복 호출 가능.
- `IsRunning()`, `GetFrequency()`: 실행 상태와 마지막 설정 주파수 조회.
- 실행 중 또는 미회수 worker가 있으면 Create는 실패한다. Stop 후 Create로 다시 사용할 수 있다.
- frequency는 양수·유한값이며 steady_clock의 유효한 양수 period로 표현 가능해야 한다. 빈 callback은 거부한다.

Scheduler:

- Timer 하나당 독립 worker thread 하나를 사용한다.
- `std::chrono::steady_clock`과 condition_variable::wait_until의 stop predicate로 대기한다.
- next_tick += period로 절대 실행 시각을 유지해 callback 실행 시간에 따른 drift 누적을 방지한다.
- callback 이후 next_tick이 이미 지났다면 now + period로 재정렬한다. missed callback은 버리고 catch-up burst를 실행하지 않는다.
- 긴 period도 Stop 알림으로 즉시 대기에서 깨어난다. 실행 중 callback을 강제 중단하지 않고 반환을 기다린다.
- callback 예외가 worker 밖으로 전파되지 않도록 worker를 정지시키며 IsRunning은 false가 된다.

Threading과 수명:

- wait 내부 mutex를 해제한 후 callback을 실행하며 callback 실행 중 Timer 내부 lock을 보유하지 않는다.
- ProcessElement::Loop, Subscriber callback, Timer callback은 서로 다른 execution context일 수 있다.
- application state를 공유하면 사용자가 thread safety를 확보한다.
- 외부 Create/Stop/destructor는 controlling thread에서 직렬화한다. callback은 자기 Timer를 파괴하지 않는다.
- callback 내부 Stop은 stop 요청만 수행한다. 자기 thread를 join하지 않으며 이후 외부 Stop/destructor가 회수한다.
- destructor는 Stop으로 worker만 정리한다. callback의 application resource 정리는 사용자의 책임이다.
- Timer callback에서 Publish하는 경우 Timer Stop을 완료한 뒤 Publisher를 Close/Unlink한다.

ProcessRuntime의 Setup/Loop/Shutdown 및 기존 loop scheduler는 유지한다.
Timer를 Runtime scheduler와 통합하지 않는다. Topic의 공유 notification과 Timer의 로컬 condition variable은 별개이다.
TimerManager, thread pool, Executor, Runtime callback queue, realtime policy, affinity,
statistics framework 및 이후 Service/Action/Parameter 기능은 추가하지 않는다.

`kcf_timer_test`는 2 Hz / 100 Hz 주기, 긴 period Stop, overrun과 Timer → Publisher → Subscriber를 검증한다.
이 문서는 기존과 같이 로컬 개발 문서로 유지하며 Git ignore 상태를 변경하지 않는다.


## Phase 2 Integration Verification (Phase 2-6)

- `examples/integration`은 기존 public API만 사용하는 Backend Element와 별도 Frontend/Test Client이다.
- ProcessRuntime의 100 Hz Loop에서 Count Action을 진행하고, 50 Hz Timer가 State Topic을 발행한다.
- Setup에서 Command Subscriber, State Publisher/Timer, ServiceServer, Runtime Parameter, ActionServer를 등록한다.
- Client가 Command Topic을 소유한다. Backend는 State Topic을 먼저 만들고 Command 생성을 최대 10초 기다리며, Client도 연결 준비를 최대 10초 기다린다.
- 하나의 localhost UDP port 22100에 Add/SetValue(1/2)와 Action Goal/Cancel/GetResult(100/101/102)를 등록한다.
- Action RUNNING 중에도 Command, State Timer, Service, Parameter Get/Set/callback이 정상 동작함을 확인했다.
- application state는 짧은 mutex로 보호한다. 취소는 Loop를 차단하지 않는 100ms 종료 준비 후 Canceled로 완료한다.
- Shutdown에서 Timer/Subscriber/Parameter/Service worker를 종료하고 owner가 SHM을 명시적으로 Unlink한다. Setup 실패 시 생성된 자원도 정리한다.
- 직접 실행 1회와 기존 Bringup 실행 9회, 총 10회 lifecycle 검증 PASS. Bringup SIGINT → child SIGTERM → Runtime Shutdown → waitpid 회수를 확인했다.
- 반복 실행 중 Backend/Client FD 수는 8/9로 일정했고 Backend UDP socket은 하나였다. 종료 후 child/zombie/테스트 SHM 잔류 없이 port 재사용이 가능했다.
- 기존 Topic 1:N, Timer, Service, Parameter, Action 독립 예제 회귀 검증도 모두 PASS했다.
- 기존 Framework API, Bringup 구조, IPC primitive는 변경하지 않았다.

실행: Backend를 먼저 시작하고 10초 이내 별도 터미널에서 Client를 실행한다.

```sh
./build/bringup/kcf_bringup ./build/examples/integration/kcf_integration_backend
./build/examples/integration/kcf_integration_client
```

Client PASS 후 Bringup에서 Ctrl+C로 종료한다. 직접 실행은 `./build/examples/integration/kcf_integration_backend`를 사용한다.

## v3.0 Phase 3-1 — Element Independence / Multi-Element Supervisor Base

- 각 Element는 자체 main과 독립 executable target을 가진다. 개별 build/run/test가 가능하며 Bringup도 같은 executable을 실행한다.
- `Process::Start(executable, args={})`는 shell 없이 execv로 인자를 전달한다. argv는 fork 전에 준비한다.
- CLOEXEC pipe로 exec 성공/실패를 확인한다. exec 실패는 errno를 반환하고 실패 child를 회수하므로 partial startup failure와 실행 중 종료를 구분한다. 이는 application Setup 성공을 확인하는 readiness protocol은 아니다.
- `ProcessExitInfo`는 available, exited_normally, exit_code, signaled, signal_number를 제공한다. Wait/IsRunning이 회수한 상태를 POSIX macro로 해석하며 GetExitInfo 자체는 wait하지 않는다.
- Process는 child PID 단일 owner이며 destructor는 kill하지 않는다. RequestStop/Wait 분리, 중복 reap 방지 및 stale PID signal 방지 원칙을 유지한다.
- Bringup은 `ElementSpec{name, executable, arguments}` 목록을 받아 순차 시작하고 100ms마다 모든 child를 감시한다. partial startup failure는 이미 시작된 child를 stop/reap한다.
- ManagedElement는 원래 PID, liveness, failure 및 exit reason을 보존한다. RUNNING 중 예상하지 않은 종료는 exit code 0도 failure이며 한 번만 기록한다.
- 하나의 unexpected exit가 다른 Element 또는 Bringup의 자동 종료를 유발하지 않는다. 모든 child가 종료해도 사용자 종료 요청까지 Supervisor를 유지한다. 자동 restart는 없다.
- 사용자 SIGINT/SIGTERM 시 모든 surviving child에 먼저 stop을 요청한 뒤 전체 Wait/reap하고 signal handler를 복원한다. 종료 단계의 child exit는 신규 failure로 기록하지 않는다.
- 정상 사용자 종료는 0, 실행 중 failure 기록 또는 정리 오류가 있었다면 사용자 종료 후 1을 반환한다. 종료를 요청하기 전까지 failure만으로 Run을 끝내지 않는다.
- signal handler는 sig_atomic_t flag만 변경한다. timeout/강제 종료, dependency, ApplicationState, Safe State, reset/recovery는 추가하지 않는다.

CLI: `kcf_bringup <executable> [args...] [--next <executable> [args...]]`.
기존 `kcf_bringup ./kcf_dummy`를 유지한다. `--next`는 Element 구분자이며 child에 전달하지 않는다.
기본 이름은 element1, element2 순서이고 child 인자의 `--element-name` 값이 있으면 Supervisor 로그에도 사용한다. 이름은 중복될 수 없다.

```sh
cmake --build build --target kcf_supervisor_normal
cmake --build build --target kcf_supervisor_crash
./build/examples/supervisor/kcf_supervisor_normal --element-name A
./build/bringup/kcf_bringup ./build/examples/supervisor/kcf_supervisor_normal --element-name A --next ./build/examples/supervisor/kcf_supervisor_crash --element-name B --crash-after-ms 500 --next ./build/examples/supervisor/kcf_supervisor_normal --element-name C
```

Crash executable은 테스트 전용이며 기본 SIGSEGV를 발생시킨다. `--exit-code 7` 또는 `--exit-code 0`을 추가하면 지정된 일반 process exit를 검증할 수 있다.

검증: clean build 및 두 Element 개별 target build PASS. 직접 실행 SIGINT/SIGTERM,
인자 보존, exit code 0/7 및 SIGSEGV 해석, 부분 startup 실패 정리, signal handler 복원,
A/B/C 중 B 종료 후 A/C 1초 이상 유지, 실패 1회 기록, 자동 restart 부재, 사용자 종료/reap를 확인했다.
Dummy/Topic/Timer/Service/Parameter/Action 회귀와 Integration 전체 lifecycle 10회도 PASS했다.

## v3.0 Phase 3-2 — Application State / Error Latch

- Supervisor 공통 ApplicationState는 INITIALIZING, RUNNING, ERROR, SHUTTING_DOWN이다. Process lifecycle과 별개이며 Bringup 계층에서 관리한다.
- Setup의 모든 exec 성공 후 INITIALIZING → RUNNING으로 전환한다. Setup 중 시작 실패는 부분 child 정리 후 종료하며 ApplicationError를 latch하지 않는다.
- RUNNING에서 최초 unexpected Element termination을 감지하면 ApplicationError에 이름, 시작 당시 보존한 PID, ProcessExitInfo를 저장하고 ERROR로 전환한다. 정상 process exit code 0도 unexpected termination이면 대상이다.
- 최초 원인은 감지 순서 기준이며 이후 실패로 덮어쓰지 않는다. 같은 monitor 순회에서 발견한 실패는 등록 순서대로 처리한다. 실제 종료 시각의 전역 순서를 추정하지 않는다.
- ERROR는 Shutdown이 아니다. monitor는 계속 실행하며 surviving Element는 유지한다. 이후 failure는 각 ManagedElement에 독립 기록하고 최초 origin은 그대로 둔다.
- Application ERROR와 각 Element failure/상태 transition은 한 번만 로그한다. ERROR 자동 해제, RUNNING 자동 복귀, restart, Safe State 전송은 없다.
- 사용자 종료는 RUNNING/ERROR → SHUTTING_DOWN 후 surviving child stop/reap, signal handler 복원을 수행한다. 종료 단계는 새로운 ERROR를 만들지 않고 기존 origin은 종료 후에도 보존한다.
- GetApplicationState(), GetApplicationError(), GetElementStatuses()로 조회한다. Element snapshot은 이름/PID/running/failure_detected/exit_info를 포함한다. running은 마지막 monitor 관찰값이며 query 자체가 waitpid를 호출하지 않는다.
- Lifecycle과 query는 동일 controlling thread에서 호출한다. Signal handler는 sig_atomic_t flag만 변경하며 state용 mutex/atomic은 추가하지 않는다.
- Reset API가 없으므로 종료한 Bringup 객체의 Setup 재사용은 -EBUSY이다. 새 실행 lifecycle은 새 객체로 시작하며 기존 객체의 first error는 지우지 않는다.
- System Error Topic, Reset/Initialize, Safe State, stale IPC recovery 및 persistent error history는 이번 단계에 포함하지 않는다. Process와 v2.0 통신 구현은 변경하지 않는다.

검증: clean build PASS. 정상 RUNNING, B SIGSEGV 후 ERROR 유지, C exit 7 추가 실패 후 B origin 보존,
unexpected exit 0, startup 실패의 latch 부재, RUNNING/ERROR 사용자 종료와 전체 reap를 API/실행 로그로 확인했다.
Phase 3-1 Process 인자/종료 정보/Multi-Element 회귀, Dummy/Topic/Timer/Service/Parameter/Action 및 Integration lifecycle 10회 PASS.

## v3.0 Phase 3-3 — System Status Broadcast / Element Safe-State Reaction

- `kcf::ApplicationState`는 `kcf/system/application_state.hpp`의 공통 타입이다. 기존 Bringup 타입 이름은 alias로 유지한다.
- `kcf::SystemStatus`는 88-byte trivially-copyable 표준 layout이다. sequence, state, error_valid, termination_kind, failed_pid, exit_code, signal_number와 고정 char[64] origin을 가진다. reserved byte는 0, origin은 최대 63 bytes를 복사하고 NUL 종료한다. 긴 이름은 byte 단위로 잘린다.
- Supervisor가 `/kcf/system/status`의 유일한 Publisher owner이다. Setup에서 child 시작 전 Create/INITIALIZING 발행, startup 완료 시 RUNNING 발행을 수행한다. 기존 Topic latest-value API를 그대로 사용한다.
- 새 worker/Timer 없이 기존 100ms monitor loop에서 약 10Hz 발행하며 상태 전이 직후에도 발행한다. ERROR는 latched state이므로 다음 snapshot에서도 반복된다. 중간 상태 snapshot 수신은 보장하지 않는다.
- 각 snapshot 생성 시 sequence를 증가시킨다. UINT64_MAX에 도달하면 wrap 없이 후속 발행을 중단하고 -EOVERFLOW를 기록한다. -EAGAIN은 재시도 없이 skip하며 다음 cycle에 최신 snapshot을 보낸다. 다른 IPC 오류는 중복 로그를 억제해 기록하며 새 internal-error FSM은 만들지 않는다.
- SystemStatus origin은 최초 ApplicationError에서만 변환한다. secondary failure가 발생해도 최초 이름/PID/종료 원인을 유지한다.
- `kcf_supervisor_safe`는 기존 ProcessRuntime/Subscriber를 사용하는 독립 target이다. callback은 ERROR 요청 atomic flag만 latch하고 실제 simulated output 100 → 0 및 SAFE 전환은 20Hz Element Loop에서 수행한다.
- SAFE 진입은 한 번 로그하고 자동 해제하지 않는다. SAFE 이후에도 Element process와 Loop는 실행하며 저주기 alive 로그로 output=0 유지를 확인할 수 있다.
- SystemStatus가 없으면 SafeElement 단독 실행은 명확한 Setup 실패를 반환한다. fake Supervisor나 자동 recovery를 만들지 않는다.
- 사용자 종료 시 SHUTTING_DOWN snapshot을 시도한 뒤 child stop/reap, Publisher Close/Unlink, signal handler 복원을 수행한다. partial startup failure도 같은 owner cleanup을 수행한다. 이미 존재하는 Topic의 Create 실패 시 다른 owner의 SHM을 삭제하지 않는다.

Safety Layers:

1. Supervisor: process unexpected termination 감지와 system status 전달.
2. Element: 살아 있는 application Loop/FSM에서 software safe-state 전환.
3. Device: 필요 시 독립적인 communication timeout, command/MCU watchdog, hardware fault protection.

SystemStatus는 최종 hardware safety mechanism이 아니며 Device Safety를 대체하지 않는다.
Element 자체가 죽으면 status를 처리할 수 없고, PID가 살아 있는 Loop/worker hang도 이번 fault detection 대상이 아니다.
Linux Supervisor의 SAFE 진입 검증은 1초 이내 관찰을 목표로 하며 hard real-time deadline을 보장하지 않는다.
User Reset/Initialize, Safe ACK, restart, heartbeat/watchdog 구현, Supervisor crash 후 stale SHM recovery는 후속 범위이다.

```sh
cmake --build build --target kcf_supervisor_safe
./build/bringup/kcf_bringup ./build/examples/supervisor/kcf_supervisor_safe --next ./build/examples/supervisor/kcf_supervisor_crash --crash-after-ms 500
```

Crash 이후 SAFE 유지 확인 후 Bringup에 Ctrl+C 또는 SIGTERM으로 정상 종료한다.

검증: 전체 clean build 및 SafeElement 개별 target build PASS. 실제 Topic에서 sequence 증가,
RUNNING/ERROR 반복 snapshot, secondary failure 이후 최초 B/SIGSEGV origin 보존,
최종 SHUTTING_DOWN snapshot과 Unlink를 확인했다. SafeElement는 테스트에서 예정된 crash 시점 후
약 173ms에 SAFE로 진입하고 1초 이상 output=0/process 생존을 유지했다.
정상/ERROR/partial-startup cleanup, Process 및 Phase 3-1/3-2 회귀,
Dummy/Topic/Timer/Service/Parameter/Action 검증 PASS.

## v3.0 Phase 3-4 — Crash IPC Recovery Base

### Recovery boundary

정책은 ERROR → surviving Element SAFE 유지 → 향후 User Reset → 모든 Application Element stop/reap
→ IPC 새 generation 복구 → 전체 Element 재시작/재초기화 순서이다. 이번 단계는 recovery 기반만 제공하며
Reset/Reinitialize/RESETTING, 자동 restart 또는 hot reconnect는 구현하지 않는다.
Create의 stale reclaim은 기존 owner가 죽고 이전 application generation의 peers가 종료된 controlled boundary에서 호출한다.
기존 mapping은 자동으로 새 object에 연결되지 않는다. 하나의 logical name에는 하나의 owner/recovery initiator만 둔다.

### SHM ownership / initialization

- Topic/Parameter Storage format을 2로 올리고 owner_pid를 추가했다. Open은 format/type/layout과 양수 owner PID를 검증한다.
- kill(pid,0)의 ESRCH만 dead로 판정한다. 성공/EPERM/그 외 오류와 PID 재사용은 보수적으로 reclaim을 막는다. zombie는 reap 후 검사한다. 이는 보안/인증 기능이 아니다.
- Create는 O_CREAT|O_EXCL을 유지한다. EEXIST에서 initialization flock을 nonblocking으로 확인하고 live/unknown owner는 -EEXIST로 보호한다. 동일 format의 dead owner object만 unlink 후 신규 Create를 한 번 재시도한다.
- zero/short incomplete object와 initialized=0인 dead/미기록 owner object는 active initializer flock이 없을 때만 reclaim한다. 알려지지 않은 complete format은 자동 migration/삭제하지 않는다.
- Linux /dev/shm directory의 짧은 flock 구간으로 shm_open부터 object initialization flock 획득까지를 직렬화한다. 이는 owner PID를 쓰기 전의 live creator race를 닫기 위한 것이다. 별도 persistent lock file은 만들지 않는다. 실제 초기화는 기존 object flock으로 보호하며 publish/get/set payload path에는 이 lock을 사용하지 않는다.
- 모든 generation peers는 동일 build/type/ABI를 사용한다. v2 stale format migration은 제공하지 않는다.

### Robust mutex / waits

- Topic notify mutex와 Parameter mutex는 PTHREAD_PROCESS_SHARED + PTHREAD_MUTEX_ROBUST이다.
- 일반 lock과 condition wait의 재획득 모두 EOWNERDEAD를 복구한 뒤 pthread_mutex_consistent를 호출한다. ENOTRECOVERABLE은 음수 errno로 반환하며 소유하지 않은 mutex를 unlock하지 않는다.
- Topic notify recovery는 notify_sequence를 atomic publish_sequence에 맞춘다. Atomic Triple Buffer, reader pin, mutex 없는 payload memcpy 및 -EAGAIN semantics를 유지한다.
- 사망한 locker가 broadcast하지 못할 수 있으므로 condition은 CLOCK_MONOTONIC 기준 최대 250ms timed wait로 predicate/owner-death를 재확인한다. 정상 알림에는 즉시 반응하며 heartbeat/health monitor가 아니다.
- StopWait도 owner-death recovery 후 stop/broadcast한다. Subscriber/Parameter Close는 ENOTRECOVERABLE인 경우에도 timed waiter를 join하고 mapping을 정리한 뒤 오류를 반환한다.
- crash로 남은 reader pin은 per-reader registry/lease로 회수하지 않는다. 전체 old generation을 정리한 뒤 새 SHM에서 초기화한다. ERROR 중 무제한 정상 운전을 보장하는 설계가 아니다.

### Parameter transaction

- 두 value slot과 active_index/version, previous_active/previous_version, lock-free atomic transaction_active를 사용한다.
- robust mutex 아래 이전 committed metadata 저장 → transaction marker 설정 → inactive slot 복사 → active_index/version 교체 → marker clear(commit) 순서이다. Marker와 memory ordering으로 부분 복사가 commit보다 뒤로 밀리지 않도록 한다.
- EOWNERDEAD에서 marker가 남으면 이전 active/version으로 rollback한다. marker clear 후 unlock 전에 죽으면 새 commit을 유지한다. 복구 도중 다시 죽어도 같은 rollback을 반복할 수 있다.
- Get/Wait는 동일 mutex 아래 committed active slot과 version을 함께 읽는다. torn inactive slot은 정상값으로 노출하지 않는다.

### 확인된 기존 오류의 최소 수정

Action Status의 기존 -EAGAIN 무한 재시도는 crash-leaked reader pin 두 개가 남으면 Start/Shutdown 진행을 막았다.
테스트에서 2초 초과 정지를 재현하여, Action 발행은 한 번 시도 후 -EAGAIN을 반환하도록 수정했다.
상태/Result commit은 유지되며 이후 API의 발행 시도나 새 generation에서 최신 상태를 전달한다. Service transport는 변경하지 않는다.

### Recovery tests

`examples/recovery`의 독립 target `kcf_topic_recovery_test`, `kcf_parameter_recovery_test`를 제공한다.
각 테스트는 timeout을 두고 20회 owner SIGKILL/reap → peer 종료 → stale Create → 새 통신 → Close/Unlink를 반복한다.
Test-only raw layout fixture로 robust owner death, condition 재획득, rollback/commit 경계, 초기화 중 죽음을 재현한다.
Parameter는 추가로 1 MiB POD를 public Set하는 client를 30회 다양한 시점에 SIGKILL하여 committed value/version 무결성을 확인한다.
Production API에는 sleep/crash hook을 넣지 않는다.

검증 결과: Topic/Parameter 각 20회 generation recovery PASS, Parameter 실제 Set 중 30회 SIGKILL에서
value/version 일관성과 무한 block 부재를 확인했다. active owner 및 incomplete initializer 보호,
초기화 crash reclaim, lock/cond wait EOWNERDEAD, ENOTRECOVERABLE 반환·join, FD/SHM 정리를 검증했다.
Action pin 누수 재현은 수정 후 즉시 -EAGAIN 반환 및 cleanup PASS로 전환됐다.
전체 clean build, Topic 1:N/느린 latest-value 수신, Timer/Service/Parameter/Action,
Integration lifecycle 10회 회귀를 통과했다.

의도적 SIGSEGV 테스트에서 호스트 Apport core collector가 실제 종료/reap를 지연하여 예정 시각 기반
first-origin/latency 검증이 불안정했다. 테스트용 crash executable에만 PR_SET_DUMPABLE=0을 적용했다.
실제 Supervisor의 origin은 예정된 crash 시각이 아니라 최초 감지 순서이며 production fault 정책은 변경하지 않는다.

## v3.0 Phase 3-5 — Runtime Supervision Channel / Health Monitoring

Process는 Start마다 AF_UNIX/SOCK_SEQPACKET socketpair를 만들고 parent endpoint를 소유한다.
child endpoint 번호는 pre-fork에 준비한 KCF_SUPERVISION_FD 환경으로 execve에 전달한다.
부모 endpoint는 CLOEXEC, child endpoint만 exec를 통과하며 Runtime은 환경을 소비하고 CLOEXEC를 복구한다.
argv/environment allocation은 fork 전에 수행한다. 기존 exec-error pipe는 exec 실패 전달 전용으로 유지하고
별도 READY pipe/message는 사용하지 않는다. Process의 종료 수집·실패·소멸은 socket을 닫으며 소멸자는 kill/reap하지 않는다.

Runtime은 Element Setup 전에 supervision worker 하나를 시작한다. standalone 실행에는 socket/worker가 없다.
Worker는 atomic ProcessState, loop_heartbeat, runtime_error만 읽고 Element 함수를 호출하지 않는다.
Loop가 정상 반환한 후 Runtime이 heartbeat를 자동 증가시킨다. control Loop에는 socket operation을 넣지 않는다.
Finalize는 worker stop, socket shutdown, join, close 순으로 정리한다. Setup/Loop 예외는 -EFAULT,
heartbeat overflow는 -EOVERFLOW로 표현한다. Setup 실패는 Shutdown을 호출하지 않는 기존 계약을 유지한다.

고정 POD request(16 bytes)/response(40 bytes)는 magic, version, packet type, request_id를 가지며
response는 PID/state/heartbeat/runtime_error를 포함한다. Process는 길이·header·PID·reserved·request_id를 검증한다.
각 child에 미완료 요청을 하나만 두고 늦은 응답의 ID를 보존한다. invalid/stale 응답은 deadline을 갱신하지 않는다.
Supervisor의 단일 context는 모든 child에 nonblocking 요청을 보낸 뒤 공통 poll 예산 20ms를 사용한다.
100ms 주기의 monitoring이므로 deadline 검출에는 monitoring 주기와 OS scheduling 지연이 추가될 수 있다.

ElementSpec은 startup_timeout_ms=5000, health_timeout_ms=2000을 제공한다. Bringup CLI에서도 각 executable 뒤
--startup-timeout-ms N / --health-timeout-ms N을 받을 수 있으며 이 옵션은 child argv에서 제거한다.
STARTING 응답을 관찰하며 모든 살아 있는 Runtime의 RUNNING 응답을 받은 뒤 Application RUNNING으로 전이한다.
Setup 실패/초기화 timeout은 startup 실패로 cleanup하고 ApplicationError를 latch하지 않는다.
startup timeout은 실패 감지 기한이다. 강제 SIGKILL 정책이 없으므로 반환하지 않는 Setup/Loop의 cleanup 완료 시간을
보장하지 않는다. 테스트의 stall은 유한하며 SIGSTOP 테스트는 cleanup에서 SIGCONT한다.

RUNNING 중 PROCESS_EXIT, RUNTIME_ERROR, STATUS_TIMEOUT, HEARTBEAT_STALL을 구분한다.
상태 응답이 없으면 STATUS_TIMEOUT, 응답이 정상이나 heartbeat가 health timeout 동안 바뀌지 않으면 HEARTBEAT_STALL이다.
health timeout은 정상 Loop 간격보다 충분히 크게 설정해야 한다. 사망이 같은 검사에서 관측되면 PROCESS_EXIT를 우선한다.
ApplicationError는 최초 감지 origin/kind/runtime_error를 보존하고 secondary failure는 Element별로 기록한다.
Health fault만으로 살아 있는 child를 kill/restart하지 않으며, 사용자 종료 시 해당 child도 stop/reap 대상이다.
SystemStatus의 failure_kind/runtime_error 확장으로 payload가 96 bytes가 되었다. 모든 peers는 동일 build/ABI를 사용해야 한다.
ERROR broadcast 및 SafeElement의 SAFE/output=0 유지 계약은 계속 적용한다.

재현 테스트: examples/supervisor의 kcf_runtime_supervision_test는 protocol/PID/request ID, 잘못된 packet,
FD inheritance/cleanup, 10회 lifecycle을 검사한다. runtime_health_checks.py는 standalone thread/socket 부재,
500ms STARTING/barrier, Setup failure/timeout, heartbeat stall, SIGSTOP timeout, runtime exception,
최초 origin 보존/SAFE/secondary crash, 20 Element thread/FD 및 1kHz Loop progression을 검사한다.

검증 결과: clean configure/build PASS. Protocol 10 lifecycle 및 parent socket disconnect 후 worker 종료·Element 생존·
사용자 SIGTERM/reap PASS. STARTING/barrier/Setup failure/startup timeout, heartbeat stall, STATUS_TIMEOUT,
Runtime ERROR, 최초 origin/SAFE/secondary crash PASS. 20개 1kHz Element에서 약 1.7초 동안 각 최소 1697회 Loop,
Supervisor 1 thread, child당 worker 1개/socket 1개를 확인했다(일반 Linux 환경의 기능 검증이며 hard real-time 보장은 아님).
Process arguments/ExitInfo, Multi-Element/latch/SystemStatus/SafeElement, Dummy/Topic/Timer/Service/Parameter/Action,
Integration 10 lifecycle 및 Topic/Parameter recovery 회귀 PASS. Integration의 supervised backend FD는 9개로 안정적이며
standalone 대비 supervision socket 1개만 추가됐다. UDP socket은 1개이고 종료 후 port 재사용/child·SHM 정리가 정상이다.

재현 명령:
```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
build/examples/supervisor/kcf_runtime_supervision_test build/examples/supervisor/kcf_supervisor_runtime_test
python3 examples/supervisor/runtime_health_checks.py build
```

## v3.0 Phase 3-6 — User Reset / Controlled Reinitialize

이 절은 이전 단계의 Reset 미구현 및 shutdown SIGKILL 미지원 제한을 대체한다.
Application lifecycle은 INITIALIZING → RUNNING → ERROR → RESETTING → RUNNING이다.
기존 상태의 wire 값은 유지하고 RESETTING=4를 추가했다. ERROR → RUNNING 직접 복귀는 없다.

Bringup::RequestReset()은 thread-safe atomic request만 기록한다. ERROR에서만 요청을 받아 한 개로 합치며,
INITIALIZING/RUNNING/RESETTING/SHUTTING_DOWN 요청은 이후 ERROR로 이월하지 않는다.
Run controlling context만 전체 Reset을 수행한다. 기존 lifecycle/query API는 같은 controlling thread에서 사용한다.
Service/UI/network/signal Reset command, 자동 Reset/retry 또는 부분 Element restart는 추가하지 않는다.

Reset은 RESETTING 즉시 publish → 모든 old Element SIGTERM → timeout이면 SIGKILL → 전체 waitpid/reap 확인
→ 같은 ElementSpec으로 전체 새 process/socketpair 시작 → 기존 Runtime RUNNING barrier 순서이다.
ElementSpec.shutdown_timeout_ms 기본값은 1500ms이며 CLI --shutdown-timeout-ms로도 지정할 수 있다.
모든 child에 SIGTERM을 먼저 보내고 하나의 poll loop로 exit/deadline을 확인한다. 무한 blocking Wait를 먼저 하지 않는다.
Process::ForceStop()은 waitpid(WNOHANG)으로 ownership/liveness를 확인한 뒤 SIGKILL하며, 이미 종료한 PID에 signal하지 않는다.
Supervisor는 reap 완료까지 확인하고 기존 supervision fd를 닫은 뒤에만 새 generation을 시작한다.
SIGKILL은 Reset/전체 cleanup 전용이며 운전 중 health fault 자체는 기존 ERROR/SAFE 정책을 유지한다.
OS가 SIGKILL을 처리하고 child를 reap할 수 있는 정상 Linux 실행 조건을 전제로 한다. Kernel의 uninterruptible sleep까지
실시간 종료 기한을 보장하는 기능은 아니다. Signal 실패 등 cleanup 오류가 있으면 새 generation 시작을 막는다.

최초 ApplicationError는 RESETTING 중에도 유지한다. per-generation PID/runtime 응답/heartbeat/timing/failure record를
초기화하고 새 process의 정상 RUNNING 응답 및 생존을 모두 확인했을 때만 error clear와 RESETTING → RUNNING을 수행한다.
Reset 성공 로그는 한 번 출력한다. 새 startup의 exec/Setup/exit/timeout/runtime 실패는 Element별 정보와 로그에 남긴다.
실패한 새 generation도 bounded cleanup/reap하고 RESETTING → ERROR로 돌아가며 기존 최초 origin을 보존한다.
다음 명시적 RequestReset 전에는 재시도하지 않는다.

SystemStatus Publisher와 SHM 객체는 Supervisor 생존 동안 유지하고 Reset 중에도 약 100ms 주기로 RESETTING을 발행한다.
Old application owner가 남긴 stale Topic/Parameter는 old peers 전체 제거 후 새 owner Create에서 Phase 3-4 정책으로 복구한다.
Reset에서 global SHM unlink나 hot reconnect를 하지 않는다. Supervisor shutdown에서만 SystemStatus Close/Unlink한다.

SafeElement는 initial output=0이다. Callback은 RUNNING 여부를 atomic으로 전달하고 ERROR 수신은 SAFE latch로 유지한다.
Loop는 RUNNING을 확인하고 SAFE가 아닌 경우에만 output=100으로 바꾼다. Shutdown도 output=0으로 정리한다.
새 generation은 INITIALIZING/RESETTING 중 비운전 상태이며 old SAFE latch를 풀지 않는다. Software SAFE와 SIGKILL은
Device communication watchdog/hardware safety를 대체하지 않는다.

RESETTING 중 SIGINT/SIGTERM은 전체 SHUTTING_DOWN을 우선한다. 진행 중인 old/new child를 같은 bounded helper로 정리하고
추가 generation을 시작하지 않는다. 정상 RUNNING 사용자 종료 및 startup failure cleanup에도 같은 helper를 사용한다.
예상된 SIGKILL cleanup은 ProcessExitInfo에 보존하지만 새로운 ApplicationError origin으로 기록하지 않는다.

테스트 target kcf_reset_test 및 examples/supervisor/reset_checks.py는 public RequestReset API를 호출한다.
Test-only file 조건으로 지연 Setup, 실패 Setup, 무한 Setup/Loop를 만들며 production Runtime에 hook을 넣지 않는다.
Crash/SIGSTOP/heartbeat stall 및 Topic owner SIGKILL 뒤 10회 전체 recovery, origin clear 시점, 동일 SystemStatus inode,
새 PID/Topic 데이터, FD/reap, exec/Setup/timeout 실패와 재Reset, old/new 정리 중 사용자 종료를 검증한다.
SafeElement 로그를 통해 RESETTING 동안 initial/alive output=0 및 RUNNING 이후 정상 output을 확인한다.

```
cmake --build build --target kcf_reset_test kcf_supervisor_safe
python3 examples/supervisor/reset_checks.py build
```

검증 결과: 전체 clean configure/build 및 신규 reset target PASS. 10회 전체 generation recovery,
SIGSTOP/무한 Loop의 SIGKILL/reap, stale Topic Create/새 PID 데이터, RESETTING output gate,
exec·Setup·무한 Setup timeout 실패 후 재Reset, old/new generation 정리 중 shutdown 우선 처리 PASS.
Runtime supervision/heartbeat/stall/timeout 및 20 Element 1kHz progression, Process arguments/ExitInfo,
ERROR latch/first origin/SystemStatus/SafeElement, Topic/Timer/Service/Parameter/Action,
Integration 10 lifecycle, Topic/Parameter 20 generation recovery 및 Parameter 30회 Set 중 crash 회귀 PASS.
SystemStatus inode 유지, Reset 성공 후 latch clear, 안정적인 FD 수와 종료 후 child/zombie/SHM 정리를 확인했다.

## v3.0 Phase 3-7 — Fault / Recovery Integration Verification

검증 범위는 Supervisor → N ProcessRuntime → RUNNING → fault → ERROR latch/SystemStatus → SAFE →
명시적 RequestReset → RESETTING → old generation 전체 reap → stale IPC recovery → new generation
전체 Runtime RUNNING → error clear이다. 자동 restart, 부분 restart 또는 새 transport는 추가하지 않는다.

재현된 결함과 최소 수정:

- Dead owner가 reap된 뒤 새 owner Create 전에 Open하면 Topic/Parameter가 옛 SHM을 정상 mapping했다.
  새 client는 이후 owner가 이름을 재생성해도 old mapping에 남을 수 있었다.
- 독립 재현에서 Topic/Parameter Open이 모두 0을 반환했다. Open의 기존 format 검증 후 owner metadata의
  PID가 ESRCH인 경우 FailOpen(-EAGAIN)으로 닫도록 두 header만 수정했다. 수정 후 재현 결과는 모두 -11이다.
- Client는 Create가 완료될 때까지 Open을 재시도할 수 있다. 이미 열린 mapping을 변경하거나 reconnect하지 않는다.
  Payload algorithm, Parameter transaction, Runtime control Loop는 변경하지 않았다.
- 이 검사는 controlled boundary에서 이미 죽고 reap된 old owner의 stale 연결을 막는다. PID 재사용/EPERM은
  기존 보수적 정책을 유지하며 owner가 Open 직후 죽을 수 있는 일반 process race를 제거하는 기능은 아니다.
- Reset fixture는 Topic뿐 아니라 Parameter owner/peer를 함께 실행하고 새 PID 데이터, Parameter 값 및 실제
  재시작된 consumer의 수신까지 검증하도록 보강했다. 각 recovery test에도 Create 전 stale Open 거부 검증을 추가했다.

검증 fixture 보정:

- 기존 200ms 예약 crash는 부하에 따라 초기화 barrier보다 먼저 발생했다. Framework의 startup failure 처리는 정상이었고,
  RUNNING fault를 검사하는 임시 latch/status fixture의 crash 시간을 늦춰 초기화와 분리했다.
- ENOTRECOVERABLE fixture에서 실제 waiter가 mutex를 먼저 정상 복구해 test의 EOWNERDEAD 단정이 실패할 수 있었다.
  Topic은 poisoning 후 worker를 시작하고 Parameter는 callback에서 worker를 대기시킨 동안 poisoning하여 순서를 확정했다.
  Production robust mutex 처리는 수정하지 않았다.

Linux 제한: SIGKILL escalation 이후에도 kernel uninterruptible sleep(D state)은 userspace에서 bounded 종료를
보장할 수 없다. 별도 workaround framework를 추가하지 않으며 Device watchdog/hardware safety는 독립적으로 필요하다.

Stale Open 수정 보완: client가 dead owner를 확인하기 전에 shared flock을 잠깐 잡으면 새 owner의
nonblocking exclusive initialization lock과 경합해 Create가 -EEXIST가 되는 경우도 반복 Reset에서 재현됐다.
완성된 동일 format header의 dead owner는 pread로 lock 전에 확인하여 -EAGAIN으로 반환하고,
정상 format 검증 후에도 재확인한다. Recovery test는 dead-owner object의 exclusive flock을 유지한 상태에서도
premature Open이 즉시 -EAGAIN으로 반환하는지 검사한다. 초기화 중 live owner 보호는 유지한다.

최종 결과: Phase 3-7 / v3.0 PASS.

- 최종 수정 후 Debug clean configure/build 및 전체 regression 재실행 PASS.
- INITIALIZING/STARTING/RUNNING barrier, PROCESS_EXIT(SIGSEGV)/RUNTIME_ERROR(-EFAULT)/STATUS_TIMEOUT/
  HEARTBEAT_STALL, 최초 origin 및 secondary ManagedElement failure 검증 PASS.
- SystemStatus INITIALIZING/RESETTING/ERROR/SHUTTING_DOWN에서 SafeElement output=0,
  RUNNING 후 정상 output, ERROR 뒤 RUNNING snapshot을 다시 받아도 SAFE 유지 PASS.
- Topic와 Parameter를 함께 사용하는 10회 mixed-fault Reset, 새 process/consumer 데이터,
  실패/timeout/exec failure 후 명시적 재Reset 및 Reset 중 old/new shutdown 우선 처리 PASS.
- Supervisor 단일 monitoring thread, 20개 Element의 worker 1개 및 1kHz control Loop progression PASS
  (테스트 구간 각 최소 1699회; hard real-time benchmark가 아님).
- Topic latest-value/1:N, Timer, Service, Parameter watcher/transaction, Action EAGAIN,
  Integration 10 lifecycle 및 UDP port 재사용 PASS.
- Topic/Parameter 각 20 generation recovery, Parameter 실제 Set 중 30회 crash,
  stale Open의 exclusive-lock 비간섭 및 FD/SHM cleanup PASS.
- 실패 재현 fixture가 남긴 정확한 세 SHM 이름은 기존 dead-owner Create recovery와 정상 Close/Unlink로 회수했다.
  최종 실제 테스트 환경에서 KCF child/SHM 잔류 없음, 포트 22000/22010/22100/22370 재사용 확인.
- Framework 변경은 SharedChannel/SharedParameter Open의 stale owner 차단 두 header로 제한했다.
  Payload, transaction, Service/Action transport, Runtime/Bringup 코드는 이번 Phase에서 변경하지 않았다.
- 공개 Readme.md / Changelog.md를 v3.0 완료 기준으로 갱신했다. docs와 build의 Git ignore는 유지한다.

대표 재현 명령은 공개 README에 기재했다. 임시 보조 검증(최초 origin 조회, SAFE snapshot replay,
Action leaked-pin EAGAIN 등)의 실행 로그는 /tmp/kcf37-verified-*.log에 기록했다.

## v3.0 Phase 3-8 — Supervisor Loss Detection / Supervised Element Shutdown

이 hardening은 기존 supervision disconnect에서 worker만 종료하던 정책을 대체한다.
유효한 KCF_SUPERVISION_FD를 획득한 Runtime만 Supervisor 생존을 감시한다. Standalone 실행에는
supervision worker/socket/timeout 검사를 추가하지 않는다. 별도 heartbeat protocol이나 Application API는 없다.

기존 RuntimeStatusRequest가 heartbeat이다. Worker 시작 시 steady clock 기준 시각을 초기화하고,
길이·magic·version·packet type·nonzero request_id 검증을 통과한 요청에서만 갱신한다.
기존 worker의 100ms bounded poll에서 SUPERVISOR_REQUEST_TIMEOUT_MS=2000을 검사한다.
Invalid packet은 deadline을 연장하지 않으며, EOF/HUP/connection loss는 -ECONNRESET,
유효 요청 timeout은 -ETIMEDOUT으로 latch한다. Header/layout과 Supervisor request 주기는 변경하지 않는다.

Worker는 atomic stop reason과 runtime_error/running 상태만 변경하며 Element 함수, callback, Device API를 호출하지 않는다.
Main Runtime 경로가 Loop를 벗어나 Shutdown/Finalize를 수행하고 nonzero Run 결과 및 ERROR 상태를 남긴다.
Stop reason은 최초 claim을 유지한다. Signal flag는 signal handler에서도 안전한 lock-free atomic을 사용한다.
이미 SIGINT/SIGTERM 또는 RequestStop으로 정상 종료 중이면 나중 HUP/timeout이 정상 종료를 오류로 덮어쓰지 않는다.
Normal Bringup shutdown 및 Reset old-generation SIGTERM 경로는 expected termination이다.

Setup 중 loss는 worker가 latch한다. Setup 성공 반환 후 loss가 이미 발생했다면 operational RUNNING/Loop에 진입하지 않고
main context에서 Shutdown 후 오류 반환한다. Setup 자체가 실패한 경우에는 기존 Shutdown 미호출 계약을 유지한다.
Control Loop에는 새로운 socket/timeout/clock/mutex 동작을 넣지 않는다. 종료 반응 시간은 실행 중인 Setup/Loop,
기존 scheduler 대기와 Shutdown이 반환하는 시간 및 OS scheduling에 의존한다.

Setup/Loop가 무한 block되면 loss 감지는 가능해도 main-context Shutdown을 강제로 실행할 수 없다.
Worker는 pthread_cancel, async thread kill, self SIGKILL이나 Device 직접 제어를 하지 않는다.
Setup 단계 output 비활성화, Device communication watchdog, external process manager가 이 제한을 보완해야 한다.
Kernel uninterruptible sleep(D state) 및 Linux/SBC 고장에 userspace 종료/복구 기한을 보장하지 않는다.

Safety 책임:

- Element fault → Supervisor 감지 → Application ERROR → surviving Element SAFE → explicit User Reset.
- Supervisor crash/hang → socket loss/request 중단 → supervised Runtime의 자체 종료 → 현재 generation 정리.
  외부 OS manager가 새 Bringup을 실행할 수 있으나 이는 KCF 외부 정책이다.
- Linux/kernel/SBC fault → 외부 hardware/software watchdog 책임.

KCF 내부 automatic restart는 추가하지 않는다. systemd unit, 외부 manager restart 설정, SBC reboot,
hardware watchdog은 이번 구현 범위가 아니다.

검증은 examples/supervisor/supervisor_loss_checks.py를 사용한다. Linux test subreaper가 실제 Bringup SIGKILL 이후
종료한 child를 회수하여 orphan zombie를 남기지 않는다. 성공 경로에서는 child를 직접 kill하지 않는다.
Supervisor SIGSTOP은 child timeout 종료 후 SIGCONT하고 사용자 종료한다. Crash로 남은 SystemStatus는 정확한
해당 이름만 기존 owner metadata Create recovery와 Close/Unlink로 정리한다.

```
python3 examples/supervisor/supervisor_loss_checks.py build
```

검증 결과: Phase 3-8 PASS. 최종 clean configure/build 및 전체 회귀 PASS.
Standalone 2.5초 실행에서 socket/worker 부재·exit 0, supervised 유효 요청 지속 시 정상 실행을 확인했다.
유효 요청 없음/invalid packet 반복은 -ETIMEDOUT, socket loss는 -ECONNRESET을 반환했다.
Setup delay 중 crash/timeout은 Shutdown loops=0 및 nonzero 결과였고, Setup 중 정상 SIGTERM은 exit 0이었다.
긴 정상 Shutdown 중 HUP/timeout은 정상 종료를 덮어쓰지 않았다. 유한 Loop stall 후 loss도 main Shutdown을 수행했다.
실제 Bringup SIGKILL/SIGSTOP 및 STARTING 중 SIGKILL에서 A/B/C 모두 자체 종료·회수됐다.
테스트 성공 경로에서는 child에 직접 종료 signal을 보내지 않고, subreaper가 종료 status와 PID 부재를 확인했다.

Protocol/arguments/environment/CLOEXEC/FD lifecycle, ERROR/SAFE/최초 origin, Reset 10회,
20 Element(단일 Supervisor context, 각 1kHz Loop 최소 1704회), Topic/Timer/Service/Parameter/Action,
Topic/Parameter crash recovery, Action EAGAIN 및 Integration 10 lifecycle을 통과했다.
실제 테스트 환경에서 KCF child/SHM 잔류 없음과 UDP port 재사용을 확인했다.
Readme/Changelog는 v3.0 hardening 항목으로 갱신했고 docs/build Git ignore를 유지했다.
로그: /tmp/kcf38-regression-*.log. 다음 Phase나 외부 manager 설정은 구현하지 않았다.

## v3.0 Phase 3-9A — Persistent SystemStatus Crash Hardening

SystemStatus를 일반 Topic에서 분리했다. 일반 SharedChannel의 atomic triple buffer/reader pin은 고속 latest-value
Topic에 그대로 유지한다. Reset을 넘어 같은 object를 유지하는 SystemStatus에는 crash-leaked reader pin이 누적될 수
있으므로 기존 SharedParameter<SystemStatus>의 process-shared robust mutex/condition 및 transaction recovery를 재사용한다.
새 low-level pthread/SHM 구현, 일반 Topic mutex 전환, reader registry 또는 recovery protocol은 추가하지 않는다.

전용 API는 kcf/system/system_status_channel.hpp의 SystemStatusPublisher와 SystemStatusSubscriber이다.
Publisher는 Create(initial), Publish(current), Close, Unlink만 제공하고 Supervisor 하나가 writer를 소유한다.
Subscriber는 Open(optional callback), ReadCurrent, Close만 제공한다. 소비자에게 Parameter Set/Create/Unlink를 노출하지 않는다.
ReadCurrent는 초기값을 포함한 완전한 current snapshot을 읽는다. Optional callback은 전용 worker 하나가 기존 SharedParameter Get/Wait로
관찰하며 shared mutex 밖에서 실행한다. 초기 snapshot을 먼저 전달하고 동일 version부터 변경을 관찰하여
late subscriber도 기존 ERROR를 놓친 채 이후 RUNNING만 받지 않도록 한다. ReadCurrent로도 초기/current 값을 조회한다.
느린 callback의 중간 snapshot 생략, application 동기화 책임, callback 반환 후 Close/join 계약은 기존과 같다.

Supervisor는 child exec 전에 sequence=1의 INITIALIZING snapshot을 Create한다. 이후 transition/periodic update마다
SystemStatus.sequence를 증가시키며 uint64 overflow에서 wrap하거나 무한 재시도하지 않는다. 내부 storage version은
SystemStatus.sequence와 별개이다. Update 오류는 기존 로그 경로에서 기록하며 ENOTRECOVERABLE을 명확히 반환한다.
Reset 동안 writer/storage inode를 유지하고 기존 ERROR origin 및 RESETTING error_valid를 보존한다.
새 generation 전체 Runtime RUNNING 이후에만 error clear, 최종 Supervisor Shutdown에서만 Close/Unlink한다.

Storage 이름은 /kcf/system/status/state (실제 SHM /kcf%2Fsystem%2Fstatus%2Fstate)로 변경했다.
SYSTEM_STATUS_STORAGE 상수와 전용 wrapper를 사용하며 기존 /kcf/system/status triple-buffer와 혼용하지 않는다.
기존 payload SystemStatus layout은 유지한다. Legacy binary/SHM의 자동 migration이나 unknown format 삭제는 제공하지 않는다.
새 storage의 dead writer는 기존 owner metadata 기반 Create recovery로 복구한다. Supervisor 자체의 crash/hang은
Phase 3-8 supervision socket loss/request timeout과 Element 자체 종료 정책을 유지한다.

SafeElement 및 runtime/reset probes는 새 전용 API로 전환했다. SafeElement 초기 output=0, RUNNING 허용,
ERROR SAFE latch와 non-operational output=0 정책은 유지한다. Topic/Parameter/Runtime 구현은 이번 Phase에서 변경하지 않는다.

Test-only kcf_system_status_recovery_test는 실제 storage layout을 확인하고 child가 shared mutex를 lock한 후
ready pipe로 알린 상태에서 SIGKILL/reap한다. 동일 inode에서 20회 반복하고 writer 또는 reader의 robust recovery,
Publish 및 새/current reader의 완전한 값, FD 안정성을 확인한다. N readers, 느린 callback, callback의 ReadCurrent 재호출,
ENOTRECOVERABLE 반환 및 dead writer Create recovery도 검사한다. Production crash/sleep hook은 없다.

```
build/examples/supervisor/kcf_system_status_recovery_test
python3 examples/supervisor/reset_checks.py build
python3 examples/supervisor/supervisor_loss_checks.py build
```

Incomplete SHM Open/Create race(Phase 3-9B), Bringup library refactor, component health, systemd,
hardware watchdog 및 network/UI는 이 작업 범위에 포함하지 않는다.

최종 검증: Phase 3-9A PASS. Clean configure/build 및 전체 회귀 PASS.
초기값과 worker의 초기 callback, 동일 inode에서 dead-reader 20회, N reader/callback 재진입,
ENOTRECOVERABLE 반환·worker join, dead writer recovery 및 FD/reap/SHM 정리를 확인했다.
Reset 10회에서 같은 SystemStatus inode, RESETTING/error_valid, 새 generation 수신 및 성공 후 error clear를 검증했다.
Phase 3-8 crash/hang 테스트에 SafeElement를 포함하여 새 subscriber의 Shutdown/join 및 자체 종료·reap도 확인했다.
20 Element/1kHz regression에서 Supervisor 단일 context와 각 Loop 진행(테스트 구간 최소 1804회)을 확인했다.
일반 Topic latest-value/1:N/slow subscriber, Timer, Service, Parameter watcher 및 1MiB transaction recovery,
Action EAGAIN/feedback/result, Topic/Parameter generation recovery와 Integration 10 lifecycle 회귀를 통과했다.
일반 Topic/Parameter 파일은 작업 전후 hash가 동일하다. 실제 테스트 환경에서 KCF child/SHM 잔류 없음,
UDP port 22000/22010/22100/22390 재사용을 확인했다.

SIGSTOP health fixture는 startup 응답 직후 queued heartbeat=0 때문에 HEARTBEAT_STALL이 먼저 검출될 수 있어
정상 Loop가 진행된 후 fault를 주입하도록 warmup을 추가했다. Runtime/Supervisor health 판정은 변경하지 않았다.
Readme/Changelog에 v3.0 hardening을 기록했으며 docs/build Git ignore를 유지한다.
검증 로그는 /tmp/kcf39a-final-*.log이며 Phase 3-9B는 시작하지 않았다.

## v3.0 Phase 3-9B — Incomplete SHM Open/Create Recovery Race

TEST-FIRST로 기존 production header를 보관한 뒤 examples/recovery의 test-only fixture를 추가했다.
Topic/Parameter 각각 4 stage(크기 0, 짧은 header, valid OwnerHeader만 존재, 전체 storage 크기이나 initialized=0)를
child의 shm_open(O_EXCL) → LOCK_EX → ready pipe → SIGKILL/reap으로 만든다.
각 stage마다 barrier 동시 시작 100회와 결정적 interleaving 1회를 실행한다(각 transport 404 cycles).
Create는 startup과 같이 한 번만 시도하고 Consumer는 -EAGAIN/-ENOENT에서 최대 2초만 retry한다.
전체 실행은 alarm 60초로 제한하며 cycle마다 FD baseline, child reap, SHM 제거, 새 inode 및 완전한 generation payload를 확인한다.
Parameter는 fresh initial version=0, Topic은 fresh first sequence=1도 확인한다.

Race reproduced: YES. Production 수정 전 실제 Open의 LOCK_SH 성공 직후 consumer scheduling만 멈추자,
replacement Create의 기존 LOCK_EX|LOCK_NB가 실패하여 양쪽 모두 -EEXIST(-17)를 반환했다.
이 제어는 해당 test target의 linker --wrap=flock에서 실제 syscall을 실행한 뒤 수행한다.
Production sleep/crash hook, 환경 변수 분기, 가짜 syscall 결과는 없다. Ungated 동시 시작에서도 실패했다.
짧은/incomplete header가 먼저 열린 경우 Topic -EPROTO(-71), Parameter -EMSGSIZE(-90)도 발생하여
정상 recovery caller의 retry 경로를 벗어났다. 최종 fixture를 수정 전 header로 별도 /tmp 빌드해 같은 결함을 다시 확인했다.

재현 순서:

```
Owner A: incomplete header 작성 → SIGKILL → reap → initialization LOCK_EX 해제
Consumer: shm_open(old) → pread(incomplete) → LOCK_SH 성공
Owner B: Create(existing) → LOCK_EX|LOCK_NB 실패 → -EEXIST
Consumer: 크기/initialized 검사 → 실패 → close
```

최소 수정은 SharedChannel::Open과 SharedParameter::Open의 기존 pread 직후에만 적용한다.
pread 오류는 errno를 반환하고, 짧은 header 또는 initialized=0이면 LOCK_SH 없이 -EAGAIN을 반환한다.
불완전한 데이터에는 attach할 수 없으므로 owner가 살아 있어도 같은 retry 응답이 안전하다.
이 검사는 owner death를 추정하거나 SHM을 unlink/reclaim하지 않는다. Live initializer는 다음 Open에서 재시도한다.
Complete dead-owner pre-check 및 flock 뒤 size/magic/format/initialized/owner 재검사는 유지한다.
Pre-check는 attach를 승인하는 근거가 아니며, TOCTOU 안전성은 기존 post-lock 검증과 함께 유지한다.
CreateOwnedShm, owner identity/PID reuse/EPERM 처리, unknown complete format의 보수적 보호는 변경하지 않는다.
Create retry/sleep, Topic triple buffer/Publish/Read, Parameter Get/Set/transaction fast path 변경은 없다.
SystemStatus 전용 API와 Runtime/Bringup production 코드는 이번 Phase에서 변경하지 않는다.

Live initializer의 LOCK_EX 보유 중 replacement Create 거부 및 LOCK_EX가 없어도 live PID를 reclaim하지 않는 것을 확인한다.
Unknown complete magic/format, owner PID 0/-1 fixture는 Create/Open이 거부하고 inode/header가 유지되는지 검사한다.
PID reuse 또는 EPERM을 죽음으로 간주하지 않는 기존 OwnerDead 정책을 유지하며 전역 SHM cleanup은 하지 않는다.

기존 Reset fixture에 incomplete 시나리오를 추가했다. Reset generation의 B가 test-only CreateOwnedShm prefix에서
Topic/Parameter 두 header를 initialized=0으로 유지한 채 ready를 알린다. Controller가 SIGKILL하고 Supervisor가 회수한다.
해당 Reset 실패 시 ERROR/origin이 유지되며 다음 명시적 Reset에서 B가 stale object를 복구하고 A가 Open 재시도한다.
새 generation의 모든 Runtime RUNNING, fresh Topic PID/Parameter 값, SystemStatus 동일 inode 및 성공 후 error clear를 확인한다.

```
build/examples/recovery/kcf_incomplete_recovery_test topic
build/examples/recovery/kcf_incomplete_recovery_test parameter
python3 examples/supervisor/reset_checks.py build
```

재현 로그: /tmp/kcf39b-before-*.log, /tmp/kcf39b-baseline-final-*.log.
수정 후 검증 로그: /tmp/kcf39b-final-*.log. docs/build Git ignore를 유지한다.

최종 결과: Phase 3-9B PASS. Race reproduced YES / Framework modified YES.
최종 동일 fixture의 수정 전 결과는 Topic 206/404, Parameter 278/404 실패였으며,
4 stage 모두 결정적 interleaving에서 LOCK_SH=1, Create=-EEXIST를 재현했다.
수정 후 Topic/Parameter 각각 404/404 PASS이며 결정적 첫 Open의 LOCK_SH=0, replacement Create=0,
eventual Open=0 및 fresh payload/version을 확인했다. FD baseline/SHM 제거/child reap 검사 PASS.
Clean configure/build PASS. 기존 Topic recovery, Parameter 1 MiB rollback/commit 및 real Set crash 30회 PASS.
SystemStatus dead reader 20회와 persistent state, Reset 10회 및 incomplete generation Reset 회귀 PASS.
Supervisor loss SIGKILL/SIGSTOP, SafeElement 자체 Shutdown/reap, standalone 및 Runtime supervision PASS.
20 Element/1kHz에서 Supervisor 단일 thread·Element별 worker 1개와 Loop 진행(최소 1578회)을 확인했다.
Topic/Timer/Service/Parameter/Action 및 Integration 10 lifecycle PASS. KCF process/zombie·SHM 잔류 없음,
UDP 22000/22010/22100 재사용 PASS. 빌드 로그는 /tmp/kcf39b-build.log, 정리 결과는 /tmp/kcf39b-cleanup.log이다.
작업 전후 Framework SHA256 비교에서 두 Open 파일 외 변경 없음, 두 파일의 정상 fast path도 동일하다.
Readme/Changelog에 실제 behavior 변경과 결과를 짧게 기록했다. 미해결 문제 없으며 다음 Phase는 시작하지 않았다.
