# 03. IPC and Features

공통: 여기서 POD는 pointer/heap ownership이 없는 fixed-size trivially-copyable payload를 뜻합니다.
R3 field 선언에는 standard-layout도 필요합니다. Native ABI를 자동 portable serialization로 바꾸지 않습니다.
기능별 검증의 출처·실행 시점은 [06](06_VERIFICATION_AND_LIMITATIONS.md)에 모았습니다.

## Topic

**Purpose / User Decision.** [USER REQUIREMENT / DECISION] 최신값 Snapshot과 최근 N개 메시지의 순차 소비를
동일 Topic에서 제공합니다. Queue 확장은 **KCF Framework v5.1 (`dev`)** 기능입니다. 기존 v5.0 Snapshot 이력은 별도로 보존합니다.

**Implementation.** [Publisher](../kcf/include/kcf/ipc/publisher.hpp) /
[Subscriber](../kcf/include/kcf/ipc/subscriber.hpp)는 [SharedChannel](../kcf/include/kcf/ipc/shared_channel.hpp)을 사용합니다.
POSIX SHM의 Depth 기반 Bounded Ring Buffer이며, 논리 Entry마다 3개 물리 Slot을 둡니다.
기존 reader pin/CAS writer ownership 보호를 유지하여 복사 중인 Slot을 강제로 덮어쓰지 않습니다.
robust notification, incomplete storage 검사, owner recovery, Stop/join 계약도 유지합니다.

| 항목 | 계약 |
| --- | --- |
| Depth | `Publisher<T>::Create(name, depth=1)`; 양수만 허용. Publisher가 설정하고 Subscriber는 Header에서 확인 (`GetDepth()`) |
| 보관 | depth=1은 기존 최신값 Snapshot, depth=N은 최근 N개 성공한 Publish를 KEEP_LAST로 보관 |
| Cursor | Subscriber별 로컬 상태. 다른 Subscriber가 읽어도 메시지가 제거되지 않음 |
| NEXT | `Subscriber<T>::Create(name)`의 기본값; 연결 시점 이후 새 메시지부터 처리 |
| OLDEST | `Create(name, TopicStartPosition::OLDEST)`; 연결 당시 보관된 가장 오래된 메시지부터 처리 |
| ReadNext | `ReadNext(value, info)` 성공 시 다음 미처리 메시지를 반환하고 해당 Cursor만 이동 |
| ReadLatest | `ReadLatest(value, info)`는 Depth와 무관하게 최신값 반환; 순차 Cursor는 불변 |
| Callback | 기존 `Create(name, callback)` 유지. Worker는 최신값을 전달하며 중간 메시지를 건너뛸 수 있음; 순차 Cursor와 독립 |
| 누락 | 성공한 ReadNext의 `TopicReadInfo::missed`는 다음 예상 Sequence부터 반환 Sequence 전까지 덮어써진 메시지 수. OLDEST 연결 이전의 이미 유실된 이력은 포함하지 않음 |
| Publish -EAGAIN | 안전한 Write Slot이 없을 때 반환. 실패한 Publish는 Queue와 Sequence를 변경하지 않음 |
| Read -EAGAIN | 읽을 값 없음, ReadNext에 새 메시지 없음 또는 일시적인 복사 경합. 실패 시 출력/metadata와 Cursor는 불변 |

`TopicReadInfo::sequence`는 **64-bit Transport Sequence**입니다. Payload Commit 성공 시에만 증가하며
SHM 새 세대에서는 다시 시작합니다. 최대값 도달 시 Publish는 wrap하지 않고 `-EOVERFLOW`를 반환합니다.
Application의 측정 Sequence와는 별개입니다. Payload Timestamp/Validity와 측정 Sequence의 생성·판단은 Application 책임이며
KCF는 이를 해석하거나 변경하지 않습니다. `valid=false`인 Payload도 정상적으로 Publish·전달됩니다.

[DynamicTopicReader](../kcf/include/kcf/dynamic/dynamic_topic_reader.hpp)는 Depth와 무관하게 최신값을 조회합니다.
기존 Tool Echo의 polling/latest 방식도 유지하며 Queue 전체를 순차 출력하는 기능으로 바뀌지 않습니다.

**Compatibility.** [storage_layout](../kcf/include/kcf/ipc/detail/storage_layout.hpp)의 Topic SHM은 **Format 4**,
Parameter SHM은 **Format 3**입니다. 기존 Topic Format 3과 바이너리 호환되지 않습니다.
기본 Depth 및 Snapshot Source API 호환은 유지하지만, Publisher/Subscriber/Tool 등 참여 바이너리는
호환 Core 및 동일 Payload ABI로 재빌드해야 합니다. 이전 Topic SHM을 임의 삭제하거나 자동 변환하지 않습니다.
기존 세대를 종료하고 명시적인 소유자 정리·재생성 절차를 적용해야 합니다.

**Implementation Origin.** 사용자 latest-value IPC 요구 기반 AI-assisted 구현에, 사용자 Topic Queue 요청을 반영했습니다.
논리 Entry별 Triple Buffer 재사용으로 기존 pin 보호와 최신값 API를 유지했습니다.
최초 Triple-buffer/pin 알고리즘 승인 시점은 기존 기록만으로 특정하지 않습니다.

**Verified — 기존 v5.0 기록.** pubsub standalone/supervised 송수신, 양 역할 endpoint 발견, 기존 UI Echo의 다섯 field와 값 증가,
정상 종료 정리 PASS. R4.1 재생성 stale, Topic recovery 및 KT-8 fixture failure/restart/재발견도 각각 PASS.
[MANUALLY VERIFIED] 사용자 직접 PubSub subscriber SIGKILL, healthy publisher 지속, Tool Refresh,
restart/generation rediscovery 기록은 자동 fixture와 구분합니다.

**Reported — Queue 구현 완료 보고.** 전체 빌드, 신규 Queue 필수 검증 10개 영역, 기존 회귀 19개 항목,
기존 kcf_tools Backend/Topic Echo PASS. 이는 이전 문서 반영 시 재실행 없이 기록한 보고입니다.
v5.1 최종 실행 결과는 [별도 항목](06_VERIFICATION_AND_LIMITATIONS.md#v51-final-verification)에 기록합니다.
[별도 검증 기록](06_VERIFICATION_AND_LIMITATIONS.md#topic-queue-verification)을 참조합니다.

**Current Limitation.** Topic당 단일 Publisher/쓰기 thread, 복수 Subscriber입니다. 같은 로컬 객체의 ReadNext는 호출자가 직렬화하고,
Close 전에 로컬 작업을 완료해야 합니다. Dead reader의 pin은 자동 회수하지 않으며 Slot 부족은 `-EAGAIN`으로 나타납니다.
KEEP_LAST는 유실 없는 전달 보장이 아니며 메모리는 Depth당 Payload Slot 3개와 metadata가 필요합니다.
Global logical name의 ownership은 Application name으로 자동 분리되지 않습니다.
Producer 재시작/SHM 재생성에 Typed Subscriber는 기존 mapping을 유지하므로 명시적 Close/Open이 필요합니다
(Subscriber는 Close/Create). DynamicTopicReader는 named object의 교체를 `-ESTALE`로 알리며 명시적 재연결합니다.
일반 Tool Topic Publish와 callback형 Dynamic Monitor는 미구현입니다.

**Future Option.** reader-liveness 기반 pin 회수, 명시적 namespace 정책은 후보이며 현재 보장 아님.

## Parameter

**Purpose / User Decision.** [USER REQUIREMENT / DECISION] 실행 중 현재 설정값의 Get/Set과 선택적 변경 알림.

**Implementation.** [Parameter](../kcf/include/kcf/parameter/parameter.hpp)는
[SharedParameter](../kcf/include/kcf/parameter/shared_parameter.hpp) Owner/Client와 watcher를 감쌉니다.
SHM Format은 **3**으로 유지합니다. Owner Create(initial), Client Open, Get/Set, Close와 Owner Unlink. SharedParameter는 robust process-shared mutex,
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
