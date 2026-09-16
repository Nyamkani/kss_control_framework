# 03. IPC and Features

공통: 여기서 POD는 pointer/heap ownership이 없는 fixed-size trivially-copyable payload를 뜻합니다.
R3 field 선언에는 standard-layout도 필요합니다. Native ABI를 자동 portable serialization로 바꾸지 않습니다.
기능별 검증의 출처·실행 시점은 [06](06_VERIFICATION_AND_LIMITATIONS.md)에 모았습니다.

## Topic

**Purpose / User Decision.** [USER REQUIREMENT / DECISION] 연속 데이터의 최신 상태를 process 간 명시적으로 공유합니다.

**Implementation.** [Publisher](../kcf/include/kcf/ipc/publisher.hpp) /
[Subscriber](../kcf/include/kcf/ipc/subscriber.hpp)는 [SharedChannel](../kcf/include/kcf/ipc/shared_channel.hpp)을 사용합니다.
POSIX SHM, 3개 slot, reader pin/writer ownership 및 sequence를 사용한 latest/current snapshot입니다.
Subscriber worker가 callback을 호출하며 느린 reader는 중간 sample을 건너뛸 수 있습니다. 이벤트 queue가 아닙니다.
[storage_layout](../kcf/include/kcf/ipc/detail/storage_layout.hpp)의 **SHM format은 3**입니다.
DynamicTopicReader는 descriptor와 현재 named object에 바인딩하여 ReadLatest합니다.

**Implementation Origin.** 사용자 latest-value IPC 요구 기반 AI-assisted 구현. Triple-buffer/pin 세부 알고리즘의
최초 승인 시점은 이번에 확인한 기록만으로 특정하지 않습니다.

**Verified.** pubsub standalone/supervised 송수신, 양 역할 endpoint 발견, 기존 UI Echo의 다섯 field와 값 증가,
정상 종료 정리 PASS. R4.1 재생성 stale, Topic recovery 및 KT-8 fixture failure/restart/재발견도 각각 PASS.
이들을 pubsub 예제 자체의 SIGKILL 전체 scenario PASS와 혼합하지 않습니다.

**Current Limitation.** Dead reader의 pin이 남아 slot 사용을 방해할 수 있으며 publish는 -EAGAIN일 수 있습니다.
Global logical name의 Publisher ownership은 Application name으로 자동 분리되지 않습니다.
일반 Tool Topic Publish와 callback형 Dynamic Monitor는 미구현; Tool Echo는 polling입니다.

**Future Option.** reader-liveness 기반 pin 회수, 명시적 namespace 정책은 후보이며 현재 보장 아님.

## Parameter

**Purpose / User Decision.** [USER REQUIREMENT / DECISION] 실행 중 현재 설정값의 Get/Set과 선택적 변경 알림.

**Implementation.** [Parameter](../kcf/include/kcf/parameter/parameter.hpp)는
[SharedParameter](../kcf/include/kcf/parameter/shared_parameter.hpp) Owner/Client와 watcher를 감쌉니다.
Owner Create(initial), Client Open, Get/Set, Close와 Owner Unlink. SharedParameter는 robust process-shared mutex,
condition/version, 두 value slot의 transaction 정보를 사용합니다. Watcher는 등록 당시 version 이후 변경을 알리며
초기값은 Get으로 읽습니다. Callback은 내부 shared lock 밖의 worker에서 실행됩니다.
[DynamicParameterClient](../kcf/include/kcf/dynamic/dynamic_parameter_client.hpp)는 descriptor/type/layout 검증 후 Get/Set합니다.
R4.1은 열린 SHM의 device/inode identity와 현재 이름을 비교하여 unlink/recreate에 -ESTALE을 반환합니다.

**Implementation Origin.** 사용자 Runtime Parameter 요구 기반 AI-assisted 구현; R4.1은 Tool 검증 결함에 대한 corrective 구현.

**Verified.** parameter_app의 bool/int32/float/fixed array 초기값, Tool Edit/Apply/re-read, Owner Get/watcher,
Revert, invalid input, stale/restart PASS. 단일 field 편집의 나머지 field 보존도 확인했습니다.

**Current Limitation.** Get/Set과 metadata 조회는 동기 locking으로 block될 수 있습니다. Set은 whole-value replacement이며
여러 writer의 read-modify-write를 원자적으로 merge하지 않습니다. Range/unit metadata와 Tool auto-watch는 없습니다.
현재 예제에 외부 변경 명령 경로가 없어 별도 외부 변경 후 Refresh Value scenario는 생략했습니다.
R4.1 object 확인은 시점 검사이지 unlink와 전체 read/write를 원자적으로 묶는 lock이 아닙니다.

**Future Option.** bounded access, compare-and-set 또는 config metadata는 별도 설계 후보.

## Service

**Purpose / User Decision.** [USER REQUIREMENT / DECISION] 짧은 단발 request/response. Tool은 descriptor로 form을 생성합니다.

```text
Element process
└─ localhost UDP port (ServiceServer endpoint)
   ├─ service_id 1
   ├─ service_id 2
   └─ ...
```

**Implementation.** [ServiceServer](../kcf/include/kcf/service/service_server.hpp)의 Create(port), Register(id, callback),
Start/Stop. port는 Element process의 endpoint, service_id는 그 안의 개별 Service입니다.
[현재 transport](../kcf/src/service_server.cpp)는 loopback UDP에 bind하며 SO_REUSEADDR/PORT를 사용하지 않습니다.
동일 port를 여러 server process가 bind하면 충돌합니다. 하나의 worker가 callback을 순차 호출합니다.
[protocol](../kcf/include/kcf/service/service_protocol.hpp) **version 2**, payload 최대 **4096 bytes**;
request/response type/layout identity와 크기를 검사합니다. Native layout 기반으로 호환 build가 필요합니다.
Client timeout/retry와 Server의 **64-entry bounded duplicate cache**가 있으며 영구 exactly-once 저장소가 아닙니다.

**Implementation Origin.** 사용자 request/response 및 generic Tool 요구 기반 AI-assisted 구현.
수동 port는 현재 구조에서 유지한 선택이며, Action 예제 요청에서도 명시적으로 유지했습니다.

**Verified.** service_app의 descriptor form, 10+25=35, -5+2=-3, 0+0=0, int32 overflow 응답 거부,
invalid text/범위 validation, 중복 UI 클릭 차단, timeout 중 GUI 응답, stale/restart와 handler 호출 수 PASS.
서버를 SIGSTOP한 timeout 뒤 SIGCONT에서 handler가 실행되는 것도 확인했습니다.

**Current Limitation.** timeout은 응답 미수신이며 handler 미실행 보장이 아닙니다.
Tool은 추가 retry하지 않지만 KCF 내부 retry는 유지합니다. 자동 allocator, mode별 port partition,
global port registry는 없습니다. Tool의 registration 검증과 송신 사이 race는 원자적으로 막지 못합니다.
Service wire protocol에는 registration_id가 없습니다.

**Future Option.** endpoint generation을 transport에 포함하는 설계, 명시적 port 관리 도구는 미결정 후보.

## Timer

**Purpose / User Decision.** [USER REQUIREMENT / DECISION] process-local periodic callback. 외부 IPC endpoint가 아님.

**Implementation.** [Timer](../kcf/include/kcf/timer/timer.hpp)의 Create(frequency_hz, callback)은 worker 하나를 시작합니다.
첫 callback은 한 period 뒤입니다. Callback은 내부 lock 밖에서 실행됩니다. 외부 Stop은 in-flight callback 반환 후 join합니다.
Callback 내부 Stop은 self-join하지 않으므로 외부 Stop/destructor가 후속 join해야 합니다.

**Implementation Origin.** 사용자 Timer API 요구 기반 AI-assisted 구현. app 예제 callback은 atomic 증가만 수행합니다.

**Verified.** timer_app 500 ms 설정; 로그 평균 Standalone **500.0 ms**, Supervised **500.1 ms**,
restart **489.9 ms**. 로그 수신에는 Loop/출력 지연이 포함됩니다. 각 tick 1~6 순차 증가, SIGTERM/SIGINT 정리,
Shutdown 후 700 ms 카운터 불변 PASS. 기존 Timer 회귀의 in-flight Stop/self-stop/overrun도 PASS.

**Current Limitation.** hard real-time 보장 없음. endpoint registry/Tool Timer endpoint 없음. Callback이 반환하지 않으면 Stop도 기다립니다.

**Future Option.** scheduling/관찰 기능 추가는 별도 요구가 있을 때 검토; 현재 Tool endpoint 계획으로 확정하지 않음.

## Action

**Purpose / User Decision.** [USER REQUIREMENT / DECISION] 시간이 걸리는 작업의 Goal/Feedback/Result/Cancel과 단일 active goal.

**Implementation.** [ActionServer](../kcf/include/kcf/action/action_server.hpp)는 ServiceServer에 goal/cancel/result
service IDs를 등록하고 내부 Publisher로 ActionStatus를 전달합니다. 자체 progression worker는 없으며 Application Loop가
Start/PublishFeedback/Succeed/Canceled를 호출합니다. [ActionClient](../kcf/include/kcf/action/action_client.hpp)는
ServiceClient와 Subscriber worker를 사용합니다. Action state는 IDLE, ACCEPTED, RUNNING, SUCCEEDED, FAILED, CANCELED입니다.
Cancel 요청 자체가 CANCELED 완료는 아닙니다. 결과 보관은 현재 goal 중심이며 history가 아닙니다.

**Implementation Origin.** 사용자 Action lifecycle 및 app scenario 요구 기반 AI-assisted 구현.
예제의 port별 status 이름, CLI 파싱 공유는 Codex 구현 세부 선택이며 generic allocator가 아닙니다.

**Verified.** action_app target=10의 서버 feedback 1~10, SUCCEEDED/final=10/code=0;
target=50의 약 500 ms 후 Cancel, CANCELED/code=-ECANCELED, 이후 feedback 중단 PASS.
취소 final count는 타이밍 의존이며 확인 로그에는 **5**도 있습니다. 요청 예시의 4를 고정 보장으로 쓰지 않습니다.
0/10001 invalid goal 거부, 서버 종료 후 Client bounded 오류, Supervisor ERROR, 정상/crash restart 및 기존 low-level Action PASS.
Client는 두 scenario 후 idle/RUNNING을 유지합니다.

**Current Limitation.** Action 전용 introspection/Tool UI 없음. 내부 Service/Topic metadata와 Action 전용 model을 혼동하지 않습니다.
복수 concurrent goal, queue, preemption 확장은 없습니다. Feedback latest semantics상 모든 중간 전이 수신은 보장하지 않습니다.

**Future Option.** Action descriptor/Tool model과 정책 확장은 별도 설계 후보.
