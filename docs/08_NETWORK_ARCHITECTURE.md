# 08. Network IPC Common Contracts

Framework v5.2는 v5.1 Local IPC 계약을 유지한 Network 확장입니다. **현재 Remote Parameter Get/Set, Remote Service Call, Remote Action Goal/Feedback/Result/Cancel 및 UDP Remote Topic을 지원합니다.**
현재 구현은 공통 모델, UDP Discovery v2, compatibility, Local metadata 자동 수집, 공통 TCP Control transport, Remote Parameter/Service/Action과 별도의 Topic Data Plane입니다.
Discovery는 metadata만 교환합니다. 실제 Parameter/Service 요청·응답은 peer당 같은 TCP Control connection으로 처리합니다.
Network 기능은 dev commit `0a3f2a630cec209793f9ac6e99107752e9e01283`에 최초 반영되었습니다.
KCF v5.2의 5.2.0 version-alignment commit은 `d41fc2fc71d3b60b0475035ec56ac32b92ffb849`입니다.
해당 보정에서 CMake와 `FrameworkVersion` 기본값을 `5.2.0`으로 맞췄으며,
NetworkProtocolVersion 1.0, Discovery v2, Control TCP v1, Topic wire v1,
Topic Format 4, Parameter Format 3, Local Service protocol 2 및 Local API는 변경하지 않습니다.
아래 단계별 working tree 검증·commit/push 미수행·재실행 및 timing 실패 기록은 당시 이력입니다.
당시 버전 보정의 실행 결과와 혼합하지 않으며 [별도 검증 기록](06_VERIFICATION_AND_LIMITATIONS.md)을 따릅니다.

## Local-first / peer-to-peer

- Local IPC가 기본입니다. Publisher/Subscriber, Parameter, Service, Action 사용자 API를 Local/Network로 이중화하지 않습니다.
- 모든 Local Topic을 LAN에 broadcast하지 않습니다. NetworkScope의 기본값은 LOCAL입니다.
- 장치는 중앙 Master에 의존하지 않는 peer입니다. 각 인스턴스는 UDP로 peer를 발견하고 관찰한 주소 및 timeout 기반 ONLINE/LOST 상태를 process-local table에 유지합니다.
- Publisher는 기존 Publish를 호출합니다. Local Subscriber는 기존 SHM을 읽고, opt-in Gateway가 최신값을 읽어 필요한 Remote Subscriber에 별도 route로 전달합니다.
- Local API 호출 자체는 Network를 기다리지 않습니다. Gateway 설정과 descriptor/introspection metadata가 Remote 연결을 결정합니다.

## 구현 상태

| 구현됨 | 위치 / 역할 |
| --- | --- |
| Network 공통 모델 | [types.hpp](../kcf/include/kcf/network/types.hpp): identity, target, priority, scope, metadata, request/result |
| Validation / metadata predicates | [validation.hpp](../kcf/include/kcf/network/validation.hpp): 잘못된 ID·selector·type·operation·correlation 거부 |
| Control operation tag codec | [control_codec.hpp](../kcf/include/kcf/network/control_codec.hpp): 2-byte big-endian encode/decode |
| Network model 테스트 | [network_contract_test.cpp](../examples/network/src/network_contract_test.cpp) |
| Peer Discovery | [peer_discovery.hpp](../kcf/include/kcf/network/peer_discovery.hpp), [구현](../kcf/src/network/peer_discovery.cpp): multicast announce, unicast metadata, Peer Table |
| Discovery wire codec | [discovery_protocol.hpp](../kcf/include/kcf/network/discovery_protocol.hpp), [구현](../kcf/src/network/discovery_protocol.cpp): version/길이/상한 검증 |
| Compatibility | [compatibility.hpp](../kcf/include/kcf/network/compatibility.hpp): version, capability, policy, endpoint 판정; [테스트](../examples/network/src/network_compatibility_test.cpp) |
| Local metadata / Parameter codec | [local_metadata.hpp](../kcf/include/kcf/network/local_metadata.hpp), [parameter_codec.hpp](../kcf/include/kcf/network/parameter_codec.hpp) |
| TCP Control / Remote Parameter·Service | [control_node.hpp](../kcf/include/kcf/network/control_node.hpp), [control_frame.hpp](../kcf/include/kcf/network/control_frame.hpp) |
| Control 통합 테스트 / LAN 예제 | [테스트](../examples/network/src/control_parameter_test.cpp), [CLI](../examples/network/src/parameter_peer_main.cpp) |
| Discovery 테스트 / 데모 | [자동 테스트](../examples/network/src/peer_discovery_test.cpp), [CLI](../examples/network/src/peer_discovery_main.cpp) |
| Remote Action | [ControlNode](../kcf/include/kcf/network/control_node.hpp): Goal/Feedback/Result/Cancel |
| Remote Topic | [Data Plane](../kcf/include/kcf/network/topic_data_plane.hpp), [wire](../kcf/include/kcf/network/topic_protocol.hpp), [테스트](../examples/network/src/topic_data_test.cpp) |

**Not implemented:** Reliable Topic QoS, 자동 request retry, Action re-attach, Parameter/Service 실행 취소·rollback,
범용 codec registry/type schema 교환, 인증·권한·암호화. Remote Action Cancel 및 5종 Target/fan-out은 구현했습니다.
메타데이터의 REMOTE 선언만으로 네트워크 송신이 시작되지 않습니다.

## CONTROL / TOPIC와 두 단계 우선순위

| TrafficClass | Priority | 기능 |
| --- | --- | --- |
| CONTROL | HIGH | Parameter / Service / Action; discovery metadata는 분리된 Control 논리 경로 |
| TOPIC | NORMAL | Remote-enabled Topic의 지속 Payload 전송 |

Parameter/Service/Action 사이에는 추가 우선순위가 없습니다. `Classify`는 위 분류만 수행합니다.
Discovery는 Host/Application/Element/endpoint/type/exposure metadata만 공유하며 실제 Topic Payload를 싣지 않습니다.
Discovery가 Control 자원을 사용하더라도 payload data transport와 논리적으로 분리합니다.

Control과 Topic은 **서로 독립적인 bounded queue·용량·송신 budget**을 둡니다.
큰 Topic frame 전송 또는 가득 찬 Topic queue 때문에 Control enqueue/dispatch가 대기하지 않도록
worker/socket/connection 자원을 분리합니다. 하나의 ordered stream에 우선순위 숫자만 붙이는 것은
이 계약을 만족하지 않습니다. Control 내부 추가 priority나 실시간 지연 보장은 이번 범위가 아닙니다.
Control은 bounded TCP queue와 socket worker, Topic은 UDP socket/worker와 route별 latest 수신 slot을 구현했습니다. Discovery UDP worker는 별개이며 Control operation을 실행하지 않습니다.

## Host / Runtime / Endpoint identity

`kcf::network` namespace는 기존 Local 타입을 수정하지 않습니다. Core에는 별도의 Local RuntimeIdentity
구조체 대신 RuntimeInfo 등에 PID/start_ticks가 있습니다. Network 모델은 다음 필드를 별도로 조합합니다.

- `HostIdentity`: 128-bit 고정 device identity. 0은 invalid. hostname/IP는 identity가 아닙니다.
- `RemoteRuntimeIdentity`: host + boot_id + pid + process_start_ticks.
- `RemoteEndpointIdentity`: RemoteRuntimeIdentity + registration_id.

Host ID는 배포 시 고유하게 provision하여 지속 보존하고, boot_id는 부팅 세대를 구분합니다.
같은 장치가 재부팅하여 PID/start_ticks가 재사용되어도 이전 endpoint와 혼동하지 않습니다.
같은 PID/start_ticks라도 host 또는 boot_id가 다르면 다른 Runtime입니다. 등록 ID도 재등록 시 달라져야 합니다.
Library는 ID provisioning/중복 장치 자동 해결을 구현하지 않습니다. CLI 데모는 명시적 Host ID와 Linux boot_id를 사용합니다. Equality가 ID 발급의 고유성이나 인증을 보장하지 않습니다.

Application identity는 해당 Host의 **Supervisor Runtime identity**입니다. Application 이름만으로 instance를 구분하지 않습니다.
Standalone metadata는 application identity와 application_name을 모두 비웁니다. 기존 Application-scoped SystemStatus는 그대로입니다.

## 공통 RemoteTarget

Parameter/Service/Action이 같은 `RemoteTarget`을 사용하며 기능 이름과 routing selector를 분리합니다.
예: endpoint_name=`/stop`, target=ALL 또는 GROUP의 group=`mobile_robots`.
별도의 `/stop_all`, `/stop_host` Service를 만들지 않습니다.

| Scope | 반드시 채우는 selector |
| --- | --- |
| ENDPOINT | endpoint: 정확한 Runtime 세대 + registration_id |
| HOST | host: 해당 device |
| APPLICATION | application: 정확한 Supervisor instance |
| GROUP | group: 설정된 그룹 이름 |
| ALL | 없음; 현재 발견되고 조건에 맞는 대상 |

선택하지 않은 필드는 모두 비워야 하며 충돌하는 selector/알 수 없는 enum/빈 ID는 거부합니다.
HOST는 재부팅 이후에도 같은 고정 device를 의미하고, ENDPOINT/APPLICATION은 특정 boot/runtime 세대입니다.
`MatchesTarget`은 REMOTE인 non-Topic PROVIDER에만 적용합니다. `MatchesRequest`는 여기에
endpoint_name, 기능 kind 및 request/response type 호환성을 검사합니다.
이 함수들은 metadata predicate이며 discovery 조회, 살아 있는 peer 보장 또는 실제 route 생성이 아닙니다.

## Topic 정책

`NetworkScope::LOCAL`이 기본이고 `REMOTE`는 명시적 opt-in입니다. AUTO는 예약된 미래 정책이며 지금 수용하지 않습니다.
Topic에는 command target을 적용하지 않고 Publisher/Subscriber discovery를 통해 route를 형성합니다.
`CanRouteTopic`은 다음 조건을 모두 요구합니다.

1. 양 endpoint metadata가 유효하고 동일 이름·type/schema/encoding/size.
2. TOPIC PROVIDER와 TOPIC CONSUMER, 서로 다른 Host.
3. 양 endpoint가 REMOTE 허용.
4. 호출자가 확인한 active remote subscription이 존재.

Subscription의 실재 여부·lease·disconnect는 이후 route manager가 확인해야 하며 bool 인자를 liveness detector로 해석하지 않습니다.
Local Subscriber는 계속 SHM을 사용합니다. 실제 UDP route/forwarding과 proxy bootstrap은 아래 Remote Topic 절을 따릅니다.
Topic Depth/KEEP_LAST/Transport Sequence와 Payload 측정 Sequence·Timestamp·Validity의 책임은 기존 [Topic 계약](03_IPC_AND_FEATURES.md#topic)을 유지합니다.

## 공통 Control request/result

Parameter는 on-demand Get/Set과 metadata, Service는 Request/Response, Action은 Goal 실행에 연관된
Feedback/Result/Cancel을 공통 Control 경로로 처리하는 설계입니다. Parameter 값을 지속 복제하지 않습니다.
완료된 Goal의 명시적 Result 조회·응답은 가능하지만 idle 상태의 주기적 복제 stream은 만들지 않습니다.
Watch는 미래 확장입니다. 기능마다 별도 Network transport를 만들지 않습니다.

`ControlOperation`의 할당된 tag:

| 값 | Operation |
| --- | --- |
| 1 / 2 | PARAM_GET / PARAM_SET |
| 3 / 4 | SERVICE_REQUEST / SERVICE_RESPONSE |
| 5 / 6 | ACTION_GOAL / ACTION_FEEDBACK |
| 7 / 8 | ACTION_RESULT / ACTION_CANCEL |

`EncodeControlOperation`은 두 byte big-endian, `DecodeControlOperation`은 정확히 두 byte와 알려진 tag만 받습니다.
에러는 -EINVAL/-EMSGSIZE/-EPROTO이며 실패 시 출력은 불변입니다. 이 codec은 operation tag 전용입니다.
Discovery는 별도 magic/version의 wire codec을 사용합니다. 실제 Parameter Control은 아래 `ControlFrame` codec을 사용하며 기존 2-byte operation tag codec과 혼동하지 않습니다.

`RemoteRequest`는 protocol_version, source, request_id, operation, target, endpoint_name,
finite timeout_ms, payload/response type, payload를 보유합니다. Request key는 **(source, request_id)**이며
retry는 같은 key를 사용하고 새 요청은 새로운 ID를 사용해야 합니다. 다른 Host의 같은 숫자 ID는 다른 요청입니다.
상대 Host clock의 timestamp 대신 requester-local timeout duration을 사용합니다.

Parameter Set와 Action Cancel 성공은 빈 응답 + 성공 status, Get/Service는 선언된 응답 Payload를 사용합니다.
Action Goal의 response_type은 terminal result schema이며 성공 결과는 terminal completion을 뜻합니다.
수락/진행 중에는 per-endpoint PENDING을 유지하는 기반 모델입니다. 실제 Goal acceptance protocol은 TODO입니다.
Action Goal key도 (source, 초기 request_id)입니다. 후속 Result/Cancel은 독립 request_id와 필수 goal key를 가집니다.
Feedback/Result event는 `RemoteActionEvent`로 goal 및 provider endpoint에 연결하고 선언된 type을 검사합니다.
실제 상태 전이·goal 생존 여부 확인·event ordering은 아직 없습니다.

## 병렬 fan-out 및 부분 실패

ALL/GROUP/HOST/APPLICATION의 선택 결과를 요청 시작 시 고정합니다. 모든 대상의 작업을 시작한 뒤 개별 응답을 모으며
A 응답을 기다린 후 B 요청을 보내는 순차 실행 구조를 사용하지 않는 것이 executor의 계약입니다.
동시 실행 수는 resource budget으로 제한할 수 있지만 각 endpoint가 별도 timeout/result를 가집니다.

`RemoteRequestResult::endpoints`에는 선택된 endpoint마다 정확히 하나의 `RemoteEndpointResult`를 둡니다.
처음에는 PENDING이며 SUCCESS/TIMEOUT/DISCONNECTED/LOST/REJECTED/REMOTE_ERROR를 개별적으로 보존합니다.
예: request 100에서 device A는 SUCCESS, B는 TIMEOUT. 부분 실패를 하나의 성공 값으로 덮지 않습니다.

Aggregate `Valid(results, request, selected)`는 선택 snapshot과 비교하여 누락·중복·다른 요청/endpoint 결과,
잘못된 type·status·error 조합을 거부하며 결과 도착 순서는 요구하지 않습니다.
선택 결과가 비어 있으면 실패입니다. 단일 결과 validation은 구조·correlation 검사이므로 최종 수집 결과에는 aggregate validation이 필요합니다.
실제 Parameter executor는 `ParameterBatchResult` / `ParameterEndpointResult`를 반환합니다. 선택 고정, endpoint별 비동기 dispatch·deadline·결과, no-match 오류를 구현했으며 원격 cancellation은 미구현입니다. 위 `RemoteRequestResult`는 범용 모델 계약으로 유지합니다.

## Type / ABI / wire 경계

`TypeIdentity`는 type_id + wire schema_id + encoding_id + fixed payload_size입니다.
기존 R3 canonical type_id와 연계할 수 있으나 schema_id는 **Local SHM layout_id가 아닙니다**.
현재 validator는 식별자·type·길이를 검사합니다. Parameter는 검증된 Local TypeDescriptor로 아래 고정 field codec을 구성합니다. 임의 type의 범용 codec registry나 remote schema 전송은 미구현입니다.
본 단계 모델의 Payload 최대 크기는 1 MiB, endpoint name은 '/' 시작 최대 240 bytes, 그룹은 최대 64개/이름 128 bytes입니다.
모델은 std::string/vector를 포함하는 **process-local C++ 자료구조**이며 raw memcpy로 송신하면 안 됩니다.
Discovery metadata, Control framing, Parameter field 값에 명시적 wire codec을 제공합니다. Service request/response와 Action goal/feedback/result도 동일 field codec을 재사용합니다. Topic도 같은 field codec과 독립적인 UDP wire header를 사용합니다.

PROTOCOL_VERSION=1은 새로운 Network 모델용이며 기존 Local 버전과 별개입니다.
Discovery v2 및 Control wire v1은 아래처럼 별도로 정의합니다. Parameter는 type/schema/encoding/size의 정확한 일치를 요구합니다.
현재 C++ 객체의 sizeof/padding을 Network ABI로 고정하지 않습니다. Local RuntimeInfo/EndpointInfo/ServiceInfo는 변경하지 않았고
**Topic SHM Format 4, Parameter Format 3, Local Service protocol 2**를 유지합니다.

## 장애 격리 계약과 현재 검증 한계

현재 Control API는 bounded enqueue와 future를 사용하며 Local 제어 thread에서 socket send/remote wait를 수행하지 않습니다. 사용자가 future.get()으로 기다리는 위치는 직접 선택합니다.
Network backlog·disconnect·codec 실패는 Remote operation 또는 route에만 실패를 반환하고 Local IPC/ProcessRuntime에
fatal error를 강제하거나 Local SHM Commit을 rollback해서는 안 됩니다. Network 실패 처리 여부를 Application이 명시적으로 결정합니다.
Control과 Topic의 독립 capacity, peer별 resource budget, finite timeout 및 작업 취소가 실제 transport의 필수 조건입니다.

Discovery worker/socket은 별도이며 Local hook이 없으므로 기존 Local 실행 경로가 Network I/O를 기다리지 않습니다.
Discovery 중 Local IPC 및 discovery peer 단절은 자동 시험합니다. localhost Topic burst 및 막힌 proxy 아래 Control 완료를 검증했습니다. 실제 LAN latency와 hard real-time 보장은 검증하지 않았습니다.
ID/type 검사는 인증·권한 검사가 아닙니다. Host/Group/ALL 명령은 인증된 discovery와 명시적 exposure/authorization 없이 운영하지 않도록
향후 보안·배포 정책이 필요합니다. 현재는 명시적으로 노출한 Parameter Get/Set과 Service Call이 실제 실행되므로 신뢰된 LAN에서만 사용합니다. Host identity 검사는 인증이 아닙니다.

## 검증

전용 target: `cmake --build build --target kcf_network_contract_test`, 실행: `./build/examples/network/kcf_network_contract_test`.
Host/boot/Runtime/endpoint identity, 5개 target, metadata/type 거부, LOCAL/REMOTE route 조건,
HIGH/NORMAL 분류, 65,536개 operation tag의 decode 허용 범위, request/result correlation,
endpoint별 SUCCESS/TIMEOUT/PENDING/disconnect 결과 보존 및 Local ABI/format guard를 검사합니다.
2026-09-26 실제 실행 결과:

- `kcf_network_contract_test` 빌드 및 단위 테스트 PASS (경고 없음).
- 같은 테스트를 AddressSanitizer + UndefinedBehaviorSanitizer로 빌드·실행하여 PASS.
- 기존 Local 회귀 11개 항목 PASS: SHM incarnation, Service introspection, Dynamic access,
  Topic recovery, Parameter recovery, incomplete Topic, incomplete Parameter,
  legacy Service, 통신 Integration, Topic Queue, Timer.
- Integration에서 Topic/Timer/Service/Parameter 동시 동작과 Action Success/Cancel 확인.
- 기존 `r41_regressions.py`에서 위 관련 7개 case + Service/Integration만 선택해 실행했고 기존 검사 조건은 변경하지 않음.
  이는 Framework 전체 19/22개 회귀를 이번 작업에서 모두 실행했다는 의미가 아님.
- 기존 Local 구현 파일의 diff 없음. 컴파일 시 Topic Format 4, Parameter Format 3,
  Service protocol 2/Header 72 bytes, ServiceInfo 600 bytes와 RuntimeInfo identity offset 확인.

로그: `/tmp/kcf-network-build.log`, `/tmp/kcf-network-contract-build.log`, `/tmp/kcf-network-local-build.log`,
`build/network-local-validation/`, `/tmp/kcf-network-kcf_topic_queue_test.log`, `/tmp/kcf-network-kcf_timer_test.log`.
개발 환경의 임시 로그이며 repository artifact로 보존하지 않습니다.
위 결과는 공통 모델 단계의 historical record입니다. 후속 Discovery, Compatibility, Control/Parameter 검증은 아래에 각각 기록합니다.


## Peer Discovery — IPv4 LAN 구현

### Transport 선택과 lifecycle

IPv4 multicast `239.255.75.67:37651`을 기본으로 사용합니다. TTL=1, multicast loopback 활성화,
명시적 `interface_address` 설정을 지원합니다. 별도 Master나 broadcast subnet 계산이 필요 없고,
Linux loopback에서도 동일 group/port의 여러 인스턴스를 시험할 수 있어 multicast를 선택했습니다.
관리 범위 multicast인 239/8만 허용합니다. 인터넷/WAN/NAT traversal은 지원하지 않습니다.

`PeerDiscovery::Start(config, endpoints)`는 다음을 생성합니다.

- group 가입 수신 socket: SO_REUSEADDR, IP_MULTICAST_ALL=0.
- 인스턴스별 ephemeral unicast socket: announce 송신 및 상세 metadata 요청/응답.
- eventfd와 전용 worker 하나: nonblocking socket, 20 ms poll, socket당 최대 32개 datagram 처리 후 timer 확인.

같은 IP여도 서로 다른 Host ID와 unicast port로 다중 인스턴스가 동작합니다. 실제 배포는 Host ID당 한 인스턴스를 둡니다.
Local Runtime/IPC thread에 socket 작업이나 callback을 삽입하지 않습니다. Start 실패는 FD를 정리하고 -errno를 반환합니다.
`Stop()`은 poll을 깨우고 worker join 후 socket을 닫으며 반복 호출해도 됩니다. destructor도 Stop합니다.
정상 Stop도 별도 goodbye를 보내지 않으므로 다른 peer에서는 마지막 announce 이후 timeout으로 LOST가 됩니다.

Start/Stop/destruction은 호출자가 직렬화합니다. 시작 중 UpdateMetadata/GetPeers/GetStats는 thread-safe이며
짧은 process-local mutex를 사용합니다. Snapshot 조회는 allocation/copy가 있어 hard real-time API가 아닙니다.
`GetLastError()`는 Discovery의 최근 I/O 오류만 보고하고 ProcessRuntime 상태를 바꾸지 않습니다.

### Peer Table과 세대

`GetPeers()`는 HostIdentity로 구분된 `PeerInfo` snapshot을 반환합니다.
Host/boot/session ID, 관찰한 IPv4 source address/unicast port, steady_clock last_seen,
metadata revision, ONLINE/LOST, peer profile, compatibility 결과, metadata_ready 및 endpoint 목록을 제공합니다.
Compatibility가 실패해도 유효한 Discovery announce/metadata는 수용합니다. LOST 전환 시 마지막 profile/호환 판정은 진단용으로 남습니다.

기본 announce 간격 1초, peer timeout 4초, metadata retry 200 ms, 최대 64 peer입니다.
유효하고 새로운 sequence의 announce만 last_seen을 갱신합니다. 중복/역순 announce 및 metadata 응답으로 timeout이 연장되지 않습니다.
자기 Host ID는 boot/session과 무관하게 제외합니다. timeout 후 LOST로 남기되 metadata_ready=false, endpoint 목록을 비웁니다.
Table이 가득 차면 가장 오래된 LOST를 재사용하고, 전부 ONLINE이면 새 peer를 받지 않습니다.

매 Start에 random 64-bit session ID를 생성합니다. 같은 Host의 새 boot/session은 이전 인스턴스 timeout 이후 수용합니다.
살아 있는 동일 Host ID 충돌 시 최초 인스턴스를 유지하여 table이 오락가락하지 않게 합니다.
최근 8개 퇴역 session은 무시하지만 이는 인증이나 영구 replay 방지가 아닙니다.
주소/포트 변경도 기존 peer timeout 이후 수용합니다. 재시작 시 revision/metadata를 초기화하여 이전 세대 데이터와 섞지 않습니다.

### Announce / metadata 분리

Announce는 **100 bytes 고정**이며 Host/boot/session, sequence, metadata revision/총 byte 수/endpoint 수와
Framework version, Network protocol version, capability bitset을 담습니다.
상세 endpoint metadata나 Topic Payload는 multicast하지 않습니다.

처음 발견하거나 revision이 바뀌면 해당 peer의 관찰된 source address/port에 metadata를 unicast로 요청합니다.
최대 1 KiB 조각을 순차 요청하며 timeout 시 같은 request ID/offset으로 재시도합니다.
응답의 sender identity/session/source address, request ID, revision, offset, 총 크기가 모두 맞아야 수용합니다.
응답자는 이미 발견된 ONLINE peer에게만 응답하고 peer별 10 ms 응답 간격을 둡니다.
전송 누락·중복·순서 역전은 재시도 또는 무시로 처리합니다. 새 revision은 진행 중인 이전 snapshot을 폐기합니다.
전체를 받은 뒤 metadata count·identity·type·중복 endpoint를 검사하여 한 번에 공개합니다. 부분 snapshot은 조회되지 않습니다.

`UpdateMetadata`는 유효한 변경만 revision을 올리고 빠른 announce를 요청합니다. 동일 내용이면 revision을 유지합니다.
삭제는 빈 목록을 포함한 새 snapshot으로 전달합니다. 변경이 없고 metadata_ready이면 상세 재전송을 요청하지 않습니다.

`PeerDiscovery` 단독 API는 endpoint 목록을 **Start/UpdateMetadata에 명시적으로 제공**합니다.
`ControlNode`는 아래 Local introspection 수집기를 통해 이 목록을 자동 갱신합니다. 기존 NetworkEndpointMetadata를 그대로 교환하므로
Application/Supervisor identity와 이름, Runtime/Element, Topic/Parameter/Service/Action kind,
role, type/schema/encoding/size, groups 및 LOCAL/REMOTE policy를 확인할 수 있습니다.
LOCAL metadata를 제공해도 Payload forwarding 권한은 생기지 않습니다. codec ID의 실제 구현 존재 확인은 다음 단계입니다.

### Discovery wire protocol v2

Control operation tag codec과 별개입니다. 모든 정수는 big-endian, native struct memcpy를 사용하지 않습니다.
Header는 56 bytes입니다.

| Offset | 길이 | 내용 |
| --- | --- | --- |
| 0 | 4 | magic 0x4b434644 (`KCFD`) |
| 4 | 2 | Discovery version 2 |
| 6 | 2 | ANNOUNCE=1 / METADATA_REQUEST=2 / METADATA_RESPONSE=3 |
| 8 | 4 | datagram 전체 길이 |
| 12 | 4 | reserved=0 |
| 16 | 16 | sender HostIdentity |
| 32 | 16 | sender boot_id |
| 48 | 8 | sender discovery session_id |

Body:

- ANNOUNCE: sequence(u64), revision(u64), metadata_size(u32), endpoint_count(u16), reserved(u16=0),
  framework major/minor/patch(각 u16), network major/minor(각 u16), reserved(u16=0), capabilities(u64).
- METADATA_REQUEST: request_id(u64), revision(u64), offset(u32).
- METADATA_RESPONSE: request_id(u64), revision(u64), offset(u32), metadata_size(u32), chunk bytes.

Datagram 상한 1200 bytes, response는 최대 1104 bytes입니다. Snapshot 상한 256 KiB/64 endpoint,
chunk 1024 bytes, offset은 1024의 배수이며 마지막 chunk의 정확한 길이까지 검사합니다.
Snapshot은 endpoint 수(u16), 각 endpoint의 protocol/runtime/registration/application identity,
kind/role/scope/reserved, 네 type reference, 길이(u16)로 구분한 이름들 및 group 목록 순서입니다.
각 이름·group·payload type 상한은 기존 validation과 동일하고 reserved/후행 bytes/잘린 입력은 거부합니다.
세부 field 순서는 [codec 소스](../kcf/src/network/discovery_protocol.cpp)에 명시되어 있습니다.

잘못된 magic/version/전체 크기/identity/알 수 없는 종류 및 과도한 길이는 peer 등록 전에 거부합니다.
이 버전은 알 수 없는 확장 필드를 건너뛰지 않으며 wire 변경 시 version 협상이 별도로 필요합니다.
이 변경은 이전 80-byte Discovery v1 announce와 wire 호환되지 않습니다. 모든 Discovery message header는 v2를 사용하며
v1 및 알 수 없는 envelope version은 `-EPROTONOSUPPORT`로 거부합니다. v2 header의 잘린 80-byte announce도 거부합니다.
Request/response body와 endpoint snapshot schema는 그대로이며, v1 자동 변환/동시 지원은 없습니다.
**서로 다른 Network protocol/Framework version을 발견할 수 있다는 것은 공통 Discovery v2 envelope를 사용한다는 조건입니다.**
Network major=0, DISCOVERY bit 없는 announce, nonzero reserved 필드는 거부합니다.
알 수 없는 capability bit는 진단용으로 보존하지만 협상 결과에서 제외하며 사용하지 않습니다.
Discovery v2는 Topic SHM 4 / Parameter SHM 3 / Local Service protocol 2를 변경하지 않습니다.

### localhost 및 두 장치 수동 확인

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j4 --target kcf_peer_discovery kcf_peer_discovery_test
./build/examples/network/kcf_peer_discovery_test
```

같은 PC에서 별도 터미널 두 개:

```sh
./build/examples/network/kcf_peer_discovery 00000000000000000000000000000001 127.0.0.1
./build/examples/network/kcf_peer_discovery 00000000000000000000000000000002 127.0.0.1
```

데모는 Topic/Parameter/Service/Action **샘플 metadata만** 광고하며 실행 가능한 remote endpoint를 만들지 않습니다.
`ONLINE metadata=ready endpoints=4`를 확인하고 한쪽 Ctrl+C 후 약 4초 이내 상대가 LOST로 바뀌는지 확인합니다.
다시 실행하면 새 session으로 재발견됩니다. CLI 선택 인자는 `[PORT] [ANNOUNCE_MS]`입니다.

실제 두 장치에서는 각 장치에 고유하고 지속되는 32-hex Host ID를 배정하고 `127.0.0.1` 대신 그 장치의 LAN IPv4를 지정합니다.
데모는 각 Linux의 `/proc/sys/kernel/random/boot_id`와 실제 PID/start_ticks를 사용합니다.
동일 LAN/interface/group/port를 사용하며 multicast 수신과 peer 간 ephemeral UDP 응답이 가능한 네트워크가 필요합니다.
서로 ONLINE/4종 metadata 확인 → 정상 종료 timeout → 재시작 → SIGKILL timeout 순서로 검증합니다.
**실제 두 장치 검증은 미수행**이며 localhost 자동 시험과 구분합니다.

### Discovery 검증 기록

이번 Peer Discovery 구현에서 실제 실행한 결과입니다. 앞의 Network contract 검증 기록과 별도입니다.

- C++17 관련 target build: PASS.
- `kcf_network_contract_test`, `kcf_peer_discovery_test`: PASS.
- IPv4 loopback 실제 UDP로 2·3 peer 상호 발견, 자기 자신 제외, 동일 PID/start_ticks의 서로 다른 Host 구분: PASS.
- 정상 종료 및 별도 process SIGKILL 후 LOST, 새 session 재시작, 중복 announce가 timeout을 연장하지 않음: PASS.
- metadata revision 변경·분할 전송·빈 snapshot, Host/boot 불일치 거부: PASS.
- 잘린 packet/metadata, magic/version/size/identity 오류, 상한 및 무작위 입력 검사: PASS.
- peer 수 상한·LOST 교체, 반복 Start/Stop 및 실패한 Start의 FD 정리: PASS.
- Discovery 테스트 ASan/UBSan: PASS, sanitizer 오류 없음.
- 두 discovery process가 계속 announce/metadata 교환 상태를 유지하는 동안 기존 Local IPC 회귀 11개: PASS.
  `r41`, `r5`, `r4`, Topic recovery, Parameter recovery, incomplete Topic,
  incomplete Parameter, legacy Service, integration, Topic Queue, Timer를 실행했습니다.
- Discovery 테스트 process 내부에서도 Local SharedChannel publish/read 및 peer LOST 이후의 Local 접근: PASS.

Local 회귀 로그는 `build/discovery-local-validation/`, 추가 Queue/Timer 및 Discovery 로그는
`/tmp/kcf-discovery-*.log`에 생성했습니다. 전체 Framework 검증 묶음을 재실행한 결과로 해석하지 않습니다.
실제 두 LAN 장치 시험은 미수행입니다. 위 기록은 Discovery 단계의 historical record이며 당시 Control transport 및 remote operation은 검증 대상이 아니었습니다.


## Peer / Endpoint Compatibility

**DISCOVERED != COMMUNICABLE**입니다. Discovery는 존재와 metadata를 공유하며, 실제 Remote IPC는
별도 compatibility 검사를 통과해야 합니다. 아래 판정 API는 현재 ControlNode의 connection 및 Parameter 요청 경로에서도 사용합니다.

### Version과 capability

- `FrameworkVersion`: major/minor/patch, 현재 기본 5.2.0 (최초 구현 당시 5.1.0). 진단 및 제품 운용의 exact-version 정책용입니다.
- `NetworkProtocolVersion`: major/minor, 기본 1.0. 미래 Remote IPC wire 호환 계약의 버전입니다.
- Discovery envelope version은 별도 2이며, 기존 endpoint/Control 모델의 `PROTOCOL_VERSION=1`은 그 모델 schema입니다.
  Framework/Network 버전을 이 단일 schema 필드에 넣지 않습니다. 기존 모델 validation 계약도 유지합니다.
- `PeerProfile::capabilities`: 64-bit bitset. DISCOVERY, REMOTE_PARAMETER/SERVICE/ACTION/TOPIC,
  TARGET_ENDPOINT/HOST/APPLICATION/GROUP/ALL을 정의합니다.
- `PeerProfile` / 단독 `PeerDiscovery` 기본은 **DISCOVERY만** 광고합니다. `ControlNode`가 시작하면 TCP listener와 worker를 구성하고 REMOTE_PARAMETER, REMOTE_SERVICE, REMOTE_ACTION 및 구현된 5종 target capability를 추가합니다.
  REMOTE_TOPIC은 광고하지 않습니다. 테스트의 전체 capability profile은 가상 지원 선언입니다.
- profile과 로컬 policy는 Start 시 설정하고 같은 discovery session에서는 변경하지 않습니다. 다른 profile을 쓰려면 Stop/Start합니다.
  동일 session의 profile 변경 announce는 무시합니다.

### 정책과 결과

`DiscoveryConfig::compatibility_policy` 기본값은 `NETWORK_PROTOCOL`입니다. 같은 Network major의 장치 간
명시적인 capability 교집합을 사용할 수 있게 하고 Framework 버전은 진단 정보로 남기는 선택입니다.
제품 전체 버전을 고정해야 하면 `EXACT_FRAMEWORK_VERSION`으로 major/minor/patch 모두 일치를 요구할 수 있습니다.
Exact 정책도 Network major 검사를 생략하지 않습니다. 정책은 로컬 판단이며 향후 양쪽 transport가 각각 검사해야 합니다.

- Network major 불일치: `NETWORK_MAJOR_MISMATCH`.
- Exact 정책에서 Framework 버전 불일치: `FRAMEWORK_VERSION_MISMATCH`.
- 같은 Network major: minor가 달라도 기본 호환 가능. minor가 같아도 기능은 항상 양쪽 capability 교집합으로 제한합니다.
- 필요한 IPC 또는 target capability 부족: `CAPABILITY_MISSING`, `missing_capabilities`로 누락 bit 제공.
- endpoint type 불일치: `TYPE_MISMATCH`. 이름/역할/kind 불일치는 `SEMANTIC_MISMATCH`.

`CompatibilityResult`는 reason, common_capabilities, missing_capabilities와 `Compatible()`를 제공합니다.
`CompatibilityReasonName()`으로 진단 문자열을 얻습니다. Peer의 compatibility가 COMPATIBLE이어도
DISCOVERY만 지원하면 Remote IPC는 CAPABILITY_MISSING으로 거부됩니다.

### Endpoint와 route 검사

`CheckTopicCompatibility`, `CheckParameterCompatibility`, `CheckServiceCompatibility`, `CheckActionCompatibility`는
peer version 정책, 필요한 IPC 및 target capability, 동일 endpoint 이름/kind, 반대 PROVIDER/CONSUMER 역할,
양쪽 REMOTE scope와 서로 다른 Host를 검사합니다. PROVIDER는 Topic publisher / Parameter owner / Service server / Action server,
CONSUMER는 subscriber / client를 뜻합니다. local이 어느 역할이어도 검사할 수 있습니다.

- Topic/Parameter: value type.
- Service: request 및 response type.
- Action: request(goal), feedback, response(result) type.

모든 타입은 type_id뿐 아니라 schema_id, encoding_id, payload_size까지 정확히 비교합니다.
이 검사는 실제 codec 존재나 인증/권한을 보장하지 않습니다. Payload timestamp/sequence/validity는 여전히 Application 책임입니다.

일반 계약 검사에는 `CheckPeerCompatibility`, `CheckCapability`, `CheckEndpointCompatibility`를 사용합니다.
실제 Peer Table 기반 route 준비에는 다음 API를 사용합니다.

```cpp
// config는 Start에 사용한 로컬 설정, peer는 GetPeers()의 최신 snapshot입니다.
const auto result = kcf::network::CheckRouteCompatibility(
    config, peer, local_endpoint, remote_endpoint_identity,
    kcf::network::TargetScope::ENDPOINT);
if (!result.Compatible()) {
    // result.reason / result.missing_capabilities를 기록하고 route 생성을 거부합니다.
}
```

이 API는 ONLINE 여부 → peer compatibility → 완성된 metadata → host/boot binding → remote endpoint lookup →
endpoint compatibility를 검사합니다. caller가 임의로 제공한 remote type 대신 peer snapshot의 endpoint를 사용합니다.
LOST는 호환 이력과 무관하게 `PEER_LOST`로 거부하며, 미완성 metadata 및 없는 endpoint도 거부합니다.
Snapshot 자체는 지속적인 liveness 보장이 아니므로 향후 transport는 dispatch 직전 새 상태로 재검사해야 합니다.
Target selector의 실제 매칭, active Topic subscription, codec 설치와 인증/권한은 별도로 필요합니다.
기존 `CanRouteTopic` / `MatchesRequest`는 metadata predicate만이며 이 route gate를 대체하지 않습니다.

### Compatibility 검증 기록 (Control 구현 전)

이번 compatibility 작업에서 실제 실행한 결과입니다. 이전 Discovery v1 검증 기록과 별도이며 실제 remote execution은 포함하지 않습니다.

- 관련 C++17 target build: PASS, 경고 없음.
- `kcf_network_compatibility_test`: 동일 버전, 두 정책, major 거부, minor 차이의 capability 교집합,
  누락/unknown capability, 4종 endpoint 의미 및 모든 type field 불일치, 5종 target capability 검사 PASS.
- 실제 UDP 다중 instance에서 incompatible peer의 발견/metadata 보존, 정책별 진단, 독립적인 LOST 및 route 거부 PASS.
- `kcf_peer_discovery_test`: 기존 discovery 회귀 및 100-byte v2 roundtrip, legacy v1/잘린 v2/잘못된 version·profile·reserved 검사 PASS.
- `kcf_network_contract_test`: 기존 공통 모델 계약 PASS.
- Compatibility와 Discovery 테스트를 각각 ASan/UBSan으로 실행: PASS, sanitizer 오류 없음.
- Discovery v2 process 2개가 동작하는 동안 기존 Local IPC 회귀 11개 PASS:
  r41, r5, r4, Topic recovery, Parameter recovery, incomplete Topic/Parameter, legacy Service, integration, Topic Queue, Timer.
- Local 회귀 로그: `build/compatibility-local-validation/`, 추가 Queue/Timer·sanitizer 로그: `/tmp/kcf-compat-*.log`.

Local ABI 및 SHM storage 변경은 없습니다. Network process-local 공개 구조체에 필드가 추가되어 해당 사용자는 재빌드해야 합니다.
Discovery wire v2 배포는 peer 모두의 업데이트가 필요합니다. 이 Compatibility 단계 기록에서는 실제 두 장치 LAN 및 Remote IPC transport 검증을 수행하지 않았습니다. 후속 Control 검증은 아래와 같습니다.


## Local metadata 자동 수집 / Control Transport / Remote Parameter

### Host gateway와 자동 수집

`ControlNode`는 Host당 하나의 gateway입니다. 기존 Application의 ProcessRuntime/introspection과
Parameter 등록을 그대로 사용하며 Application에 Network 등록 코드를 추가하지 않습니다.
별도 backend worker가 `IntrospectionClient::ListRuntimes/ListSupervisors/ListEndpoints/GetType`을 조회합니다.
기본 250 ms 주기로 유효한 TypeDescriptor가 있는 Parameter, Topic, Service 및 Action을 수집하고,
Supervisor PID/start_ticks와 일치하는 Element에 Application identity/name과 Element name을 연결합니다.
수집 순서를 정렬하고 동일 snapshot에는 revision을 올리지 않습니다. 제거/추가/변경만 Discovery revision에 반영합니다.

- `ControlConfig::metadata.remote_parameters`: 정확한 Parameter owner 이름의 노출 허용 목록. 기본은 비어 있어 LOCAL입니다.
- `metadata.groups`: Host 설정의 그룹 목록. Application 코드의 등록은 필요 없습니다.
- `metadata.runtime_pids`: 테스트/namespace 분리용 선택 필터. 기본 empty는 Host의 모든 살아 있는 Runtime입니다.
- stable descriptor가 없거나 지원하지 않는 type은 수집하지 않습니다. 존재하지 않는 Network type을 추측하지 않습니다.
- Topic은 Publisher/Subscriber role을 보존하여 자동 수집합니다. `remote_topics`의 정확한 이름만 REMOTE이며, Data Plane은 별도로 enable해야 합니다.
- Service는 `ListServices`와 request/response `GetType`을 사용합니다. 별도 registration ID 공간은 아래 Network 전용 tag로 구분합니다. Action은 새 `ListActions` registry와 세 payload descriptor를 사용합니다. 아래 Action 절에 추가한 Local 관측 훅을 설명합니다.
- backend 조회 실패 시 stale 목록을 계속 노출하지 않고 빈 snapshot으로 갱신합니다. 상세 오류는 `GetLastError()`로 확인합니다.

### TCP connection 및 lifecycle

Discovery는 기존 **UDP multicast/unicast v2**입니다. Control은 **별도 TCP connection**이며 프레임과 queue도 별개입니다.
TCP listener를 먼저 bind/listen한 뒤 같은 숫자의 port를 Discovery의 metadata UDP socket에 지정합니다.
`PeerInfo::port`는 해당 Host의 UDP metadata port이면서 TCP Control port입니다. UDP/TCP는 별도 socket입니다.
이 규칙으로 Discovery v2 100-byte announce를 변경하지 않고 Control 주소를 제공합니다.
설정 `DiscoveryConfig::metadata_port=0`은 동적 port이며, 고정 방화벽 규칙이 필요하면 명시적으로 지정할 수 있습니다.

호환되고 TARGET_ENDPOINT 및 REMOTE_PARAMETER/SERVICE/ACTION 중 하나를 공통 지원하는 ONLINE peer 사이에는 eager connection을 사용합니다.
128-bit Host ID의 byte 순서가 **작은 Host만 connect**, 큰 Host는 accept합니다. 양방향 요청은 그 하나의 connection을 공유합니다.
동일 peer의 추가 connection은 거부합니다. HELLO에서 Discovery의 Host/boot/session/address와 호환성을 검사합니다.
Master는 없으며 Parameter/Service/Action별 socket도 만들지 않습니다. Service와 Action dispatch는 같은 connection을 사용합니다.

기본 connect/HELLO timeout 1초, reconnect 간격 250 ms입니다. peer LOST·session 교체·호환성 상실 시 connection을 닫습니다.
TCP 오류로 끊긴 connection은 재연결할 수 있지만 **이미 송신한 요청은 자동 재시도하지 않습니다**.
`Stop()`은 신규 admission을 중단하고 pending future를 취소 완료하며 worker join 및 socket 정리를 수행합니다.
worker 실패 시 Discovery 광고도 중단합니다. Stop/Start는 호출자가 직렬화합니다.

Control IO worker는 nonblocking poll로 socket을 처리합니다. Local introspection와 DynamicParameter 작업은 별도 backend worker입니다.
Local 실행 경로에 Network 요청을 기다리는 hook은 없습니다. Action 자동 수집용 Local registry 관측 훅만 Runtime registry와 ServiceServer lifecycle에 추가했습니다. 다만 기존 Local introspection/Parameter의 blocking robust mutex
계약을 그대로 사용하므로, **살아 있는 Local process가 mutex를 잡은 채 무기한 정지하면 backend 작업 및 Stop의 join도 기다릴 수 있습니다**.
강제 thread 취소나 Local recovery 단순화로 우회하지 않습니다. Network 장애만으로 Local thread에 원격 대기를 부과하지 않습니다.

### Control wire v1

`CONTROL_MAGIC=0x4b434643` (`KCFC`), wire version=1. Discovery version=2 및 peer Network protocol 1.0과 구분합니다.
모든 정수는 big-endian이며 raw struct/padding 전송은 없습니다.

- 16-byte prefix: magic(u32), wire version(u16), operation(u16), total frame size(u32), reserved(u32=0).
- 나머지 고정 필드: source Runtime(host/boot/PID/start_ticks), discovery session(u64), request_id(u64),
  resolved target Endpoint(host/boot/PID/start_ticks/registration), target scope(u8), compatibility reason(u8), reserved(u16),
  TypeIdentity(type_id/schema_id/encoding_id/payload_size), timeout_ms(u32), status(i32), name length(u16), reserved(u16), value length(u32).
- 고정 크기 합계 **172 bytes** 뒤에 name bytes와 value bytes가 이어집니다.
- Operation: HELLO=1, PARAM_GET_REQUEST=2, PARAM_GET_RESPONSE=3, PARAM_SET_REQUEST=4, PARAM_SET_RESPONSE=5. Service operation 6/7은 아래 확장 절을 따릅니다.
  기존 `ControlOperation`의 2-byte tag 값과 다른 framing용 enum입니다.
- Prefix를 먼저 검사해 body 크기를 제한합니다. frame 상한 17,408 bytes, Parameter wire value 상한 16 KiB,
  name 상한 240 bytes, request timeout 1..60,000 ms입니다.
- partial send/recv와 여러 frame의 연속 수신을 처리합니다. 잘못된 magic/version/operation/length/reserved/identity는 connection을 닫습니다.

Parameter codec은 descriptor 순서대로 primitive/고정 배열 field만 전송합니다. 다중 byte field는 big-endian,
bool은 0/1이며 padding은 제외합니다. Decode 시 Local padding/미기술 영역은 0으로 초기화합니다.
`schema_id`는 field 이름·kind·array_count의 FNV-1a 64-bit hash이며 offset/alignment는 포함하지 않습니다.
`encoding_id=1`, payload_size는 전송 field 합계입니다. 수신 측의 Local offset을 사용하므로 Local layout을 Network ABI로 취급하지 않습니다.
Field 목록은 완전하고 올바른 descriptor여야 합니다. 이 단계에서는 codec/schema를 원격 다운로드하지 않습니다.

### Parameter API, routing, fan-out

```cpp
kcf::network::ControlNode node;
kcf::network::ControlConfig config; // host/boot/interface 등 실제 장치 설정 필요
config.metadata.remote_parameters = {"/motor/kp"};
// node.Start(config);
// type과 value는 ParameterWireType / EncodeParameterValue로 구성합니다.
// auto future = node.Get(target, "/motor/kp", type, std::chrono::milliseconds(1000));
// auto result = future.get(); // Runtime 제어 Loop 밖에서 대기
```

`Get/Set`은 bounded admission 후 `future<ParameterBatchResult>`를 반환합니다. batch.error는 admission/selection 오류이며,
선택된 각 endpoint의 error/reason/value를 모두 검사해야 합니다. 일부 성공을 전체 성공으로 표현하지 않습니다.
ENDPOINT/HOST/APPLICATION/GROUP/ALL을 실제 지원하며, remote metadata에서 이름·종류·provider와 target을 매칭해 선택을 고정합니다.
다중 endpoint 요청은 이전 endpoint 응답을 기다리지 않고 각 TCP queue에 독립적으로 송신합니다.

송신 전 ONLINE → peer version → capability → metadata/identity → Parameter name/role/REMOTE/type gate를 검사합니다.
수신 측도 자체 policy, source session, 노출 설정 및 type을 다시 검사합니다. DynamicParameterClient의 기존
Open/Get/Set 경로를 재사용하며 registration·PID/start_ticks·named storage owner를 접근 직전에 재확인합니다.
열린 DynamicParameterClient는 endpoint 세대별로 유지하여 storage 교체를 자동으로 따라가지 않습니다.
기존 namespace 검사는 point-in-time 검사이므로 동시에 발생하는 unlink/recreate를 원자적으로 잠그는 새로운 계약은 아닙니다.

### Correlation, timeout, 자원 상한

요청 ID는 node 객체 내 단조 증가 u64이며 wrap 직전 `-EOVERFLOW`로 거부하고 재사용하지 않습니다.
connection/session/요청 ID뿐 아니라 응답의 endpoint/name/type/operation도 대조합니다.
중복·늦은 응답은 이미 완료된 future나 다른 요청에 연결하지 않습니다.
수신 request ID는 peer session별 high-water mark로 중복/역순 재실행을 막고 TCP 재연결 시에도 유지합니다.
이 기록은 최대 64 Host이며 새 Discovery session에서 해당 Host 기록을 초기화합니다. 응답 재전송 cache나 영속 exactly-once 처리는 아닙니다.

- TCP connection 최대 64개, connection별 송신 queue 최대 256 KiB.
- 대기 batch 최대 64개, 활성 endpoint request 최대 256개.
- backend job/reply queue 각 64개. 상한 도달은 EAGAIN 또는 connection 종료로 실패를 노출합니다.
- request timeout은 API admission부터 계산합니다. 초기 connect 대기도 이 deadline에 포함됩니다.
- timeout/연결 단절 이후 **Set이 이미 적용됐거나 뒤늦게 적용될 수 있습니다**. 자동 retry/rollback하지 않습니다.
- Stop은 미완료 future를 취소하지만 이미 실행 중인 Local Set을 되돌리지 않습니다.
- 이 queue는 HIGH Control 전용입니다. NORMAL Topic Data Plane과 공유하지 않습니다.

### localhost / 두 장치 수동 시험

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j4 --target kcf_control_parameter_test kcf_parameter_peer
./build/examples/network/kcf_control_parameter_test
```

localhost의 두 터미널에서, 먼저 owner를 실행합니다.

```sh
./build/examples/network/kcf_parameter_peer 00000000000000000000000000000001 127.0.0.1 37651 owner /network/demo
./build/examples/network/kcf_parameter_peer 00000000000000000000000000000002 127.0.0.1 37651 client /network/demo 00000000000000000000000000000001 42
```

client는 기존 값 Get → count=42 Set → Get 확인 후 종료합니다. 다시 실행하여 값 유지와 재연결을 확인합니다.
owner는 Ctrl+C/SIGTERM으로 종료합니다. 예제는 테스트 Parameter와 Local introspection을 구성하지만 별도 Network endpoint 등록은 하지 않습니다.
실제 두 장치에서는 각각 고유 Host ID와 자신의 LAN IPv4를 사용하고 같은 discovery port를 지정합니다.
UDP multicast/metadata와 peer TCP port를 허용해야 합니다. owner 종료/재시작, LAN 단절, type 불일치, LOCAL scope 거부도 확인합니다.
실제 두 LAN 장치 시험은 미수행이며 localhost process/socket 검증과 구분합니다.

### Control / Parameter 검증 기록 (Service 추가 전)

이번 작업에서 실제 실행한 결과이며 이전 단계 검증과 구분합니다.

- 관련 C++17 build 및 새 Network 소스의 `-Wall -Wextra -Wpedantic` 검사: PASS, 경고 없음.
- Control 통합: Local introspection → Application/Element/Parameter metadata 반영 및 제거/revision, 별도 owner process Get/Set: PASS.
- ENDPOINT/HOST/APPLICATION/GROUP/ALL, 40개 동시 요청, 성공·실패 혼합 fan-out의 endpoint별 결과: PASS.
- type/LOCAL scope/capability/Network version gate, LOST 거부, connect 실패/timeout, 진행 중 disconnect/reconnect: PASS.
- partial TCP frame, malformed prefix/length, 중복 Set과 재연결 후 replay 방지, 응답 역순/중복/지연 correlation: PASS.
- stale registration 및 같은 이름을 대체한 다른 process owner 거부: PASS.
- 별도 CLI process의 Get → Set → Get 및 client 재시작: PASS.
- 기존 Discovery, Compatibility, Network contract 테스트: PASS.
- 두 Control gateway가 실행 중인 상태에서 Local IPC 회귀 11개: PASS.
  r41, r5, r4, Topic/Parameter recovery, incomplete Topic/Parameter, legacy Service, integration, Topic Queue, Timer.
- KCF core까지 instrumentation한 ASan/UBSan 빌드의 Control, Discovery, Compatibility: PASS.

로그: `build/control-local-validation/`, `/tmp/kcf-control-*.log`.
Local ABI/storage는 변경하지 않았습니다. 실제 두 장치 LAN 시험은 미수행입니다.
위 기록은 Parameter 단계의 historical record이며 당시 Remote Service는 미구현이었습니다. 후속 Service 검증은 아래에 기록합니다.


## Remote Service — 공통 TCP Control 확장

### Metadata와 노출 설정

`MetadataConfig::remote_services`에 정확한 Service 이름을 지정합니다. 기본은 빈 목록(LOCAL)입니다.
기존 `ServiceServer::Register(id, name, callback)` / introspection 등록을 그대로 사용하며 Application Network 등록은 추가하지 않습니다.
`ListServices`와 request/response descriptor를 조회하고 Runtime PID/start_ticks, Application/Element identity를 연결합니다.
이름이나 stable descriptor가 없는 legacy Service는 원격 호출 대상으로 추측하지 않습니다.

Local Endpoint registry와 Service registry의 registration ID가 같은 Runtime 안에서도 충돌할 수 있으므로,
자동 collector는 Service의 **Network registration_id에 bit 63을 설정**하고 실제 Local registration ID를 별도 보관합니다.
Parameter/Topic ID는 유지합니다. 후속 Action namespace는 bit 62를 사용합니다. Parameter/Topic/Action Local ID는 2^62 미만, Service Local ID는 2^63 미만이어야 하며 넘으면 EOVERFLOW로 거부합니다. truncate/hash하지 않습니다.
Local registry/SHM/Service protocol은 바꾸지 않습니다. metadata 제거/재등록은 새로운 snapshot/revision으로 전달합니다.

### Operation과 wire payload

Control TCP v1의 prefix와 기존 HELLO/Parameter 172-byte 고정 부분은 그대로 유지합니다.
새 operation은 SERVICE_REQUEST=6, SERVICE_RESPONSE=7입니다. 별도 Service TCP socket은 없습니다.

Service frame은 기존 고정 부분 뒤, name/payload 앞에 **response TypeIdentity 24 bytes**를 추가합니다.
따라서 Service 고정 부분은 196 bytes입니다. 기존 `type`은 request TypeIdentity를 나타내며 response에서도 correlation용으로 유지합니다.
추가 `response_type`은 기대 response type과 실제 응답 type입니다. 응답의 endpoint는 실제 선택된 server endpoint입니다.
request에는 request_id/source/session/resolved target/name/양쪽 type/payload/deadline이 들어가고,
response는 request_id/endpoint/name/양쪽 type/status/response payload를 반환합니다.

정수는 big-endian이며 Parameter와 동일한 descriptor field codec을 사용합니다. native struct padding은 전송하지 않습니다.
Network frame 상한 17,408 bytes 및 codec value 상한 16 KiB는 유지하지만,
실제 Local Service request/response native payload는 기존 **4,096 bytes** 상한을 따릅니다.
잘린 response type, malformed header/operation/length, payload 크기 불일치는 거부합니다.
Parameter/HELLO에 Service response_type을 넣는 것도 허용하지 않습니다.

기존 Parameter-only peer의 framing은 바뀌지 않습니다. REMOTE_SERVICE를 광고하지 않는 peer에는 operation 6/7을 보내지 않으며,
구버전 decoder는 알 수 없는 operation을 거부합니다. Discovery v2와 endpoint metadata wire schema도 유지합니다.

### API와 Local Service 연결

```cpp
config.metadata.remote_services = {"/stop", "/calc"};
// node.Start(config);
// request/response TypeIdentity는 ParameterWireType(descriptor, type)으로 구성합니다.
// request bytes는 EncodeParameterValue로 만듭니다.
auto future = node.CallService(target, "/calc", request_type, response_type,
                               request_bytes, std::chrono::milliseconds(1000));
auto batch = future.get(); // Runtime 제어 Loop 밖에서 대기
// batch.error와 모든 batch.endpoints[i].error/reason/value/execution_unknown 검사
```

`ServiceBatchResult` / `ServiceEndpointResult`는 기존 endpoint별 Control 결과 구조의 별칭입니다.
Parameter API는 그대로 유지합니다. Peer ONLINE, version/capability, Service role/name/REMOTE scope와
request/response type 전체(type/schema/encoding/size)를 송신 직전에 검사합니다. Receiver도 같은 Service compatibility를 검사합니다.

실행 직전 `ValidateLocalServiceEndpoint`가 PID/start_ticks, registration ID, 이름, port/service_id,
request/response type·size를 기존 registry에서 다시 확인합니다. 제거·종료된 owner는 실행하지 않습니다.
이후 **기존 DynamicServiceClient → Local ServiceServer**로 호출하며 Local retry_count=0입니다.
해당 Local UDP socket은 기존 Service API가 쓰는 loopback 실행 경로이며 새로운 Network Service transport가 아닙니다.
Local registry 검사는 point-in-time입니다. registry 검사와 UDP dispatch 사이의 동시 port 재사용을 원자적으로 잠그는
새 owner handshake는 추가하지 않았으며 기존 Local protocol 2의 한계를 유지합니다.

### 독립 실행과 Target

ENDPOINT/HOST/APPLICATION/GROUP/ALL resolver, request ID, connection, TCP queue를 Parameter와 공유합니다.
동결된 target별 요청을 비동기로 송신하고 endpoint별 결과를 보존합니다.
Receiver는 최대 **32개의 독립 Service 작업 슬롯**을 사용합니다. 슬롯이 없으면 EAGAIN 응답을 반환하며 다른 Service timeout을 기다리지 않습니다.
각 작업은 별도 DynamicServiceClient를 사용하므로 느린 endpoint가 다른 endpoint의 Local call을 직렬화하지 않습니다.
단, 같은 Local ServiceServer의 callback 직렬 실행이라는 기존 계약은 그대로입니다.
Service 작업은 TCP IO/metadata/Parameter worker에서 callback 응답을 기다리지 않습니다.
Stop은 pending future를 취소하고 Service 작업 thread도 join합니다. 이미 시작한 Local call은 요청의 남은 timeout(최대 60초)까지 기다릴 수 있습니다.

ControlNode는 시작 시 REMOTE_SERVICE를 실제 실행 경로와 함께 광고합니다. DISCOVERY/REMOTE_PARAMETER/5종 Target은 유지하고,
현재 후속 Action 확장으로 REMOTE_ACTION도 광고하며 REMOTE_TOPIC은 광고하지 않습니다.

### Timeout/disconnect의 실행 의미

**Timeout 또는 disconnect는 Service가 실행되지 않았다는 뜻이 아닙니다.**
서버 callback이 실행된 뒤 응답 경로만 끊겼을 수 있고, timeout 뒤 callback이 완료될 수도 있습니다.
`execution_unknown=true`는 Service 요청을 송신한 뒤 오류가 발생하여 실행 여부를 보수적으로 불확실하게 표시합니다.
사전 compatibility 거부처럼 송신하지 않은 요청에는 false입니다. 송신 후 오류에 대해 이 값이 true라고 해서 실행됐다는 증명도 아닙니다.

Remote 및 Local 호출 모두 **자동 재시도하지 않습니다**. `/stop`처럼 idempotent인 Service에도 공통 계층이 임의 retry를 넣지 않습니다.
connection 재연결은 새 request만 허용하며 이전 Service를 다시 실행하지 않습니다.
늦은/중복 response는 이미 완료된 future 또는 다른 request에 연결하지 않습니다. cancellation/rollback/영속 exactly-once 처리는 미구현입니다.

### 두 장치 수동 시험

기존 예제 executable에 service-owner/service-client 모드를 추가했습니다.

```sh
cmake --build build -j4 --target kcf_parameter_peer kcf_control_service_test
./build/examples/network/kcf_control_service_test
# 장치 A: 127.0.0.1 대신 A의 LAN IPv4
./build/examples/network/kcf_parameter_peer 00000000000000000000000000000001 127.0.0.1 37651 service-owner /network/calc
# 장치 B: 127.0.0.1 대신 B의 LAN IPv4
./build/examples/network/kcf_parameter_peer 00000000000000000000000000000002 127.0.0.1 37651 service-client /network/calc 00000000000000000000000000000001 21
```

성공 시 count=42, gain=3을 확인합니다. client를 재실행하고 owner 종료/재시작 및 LAN 단절을 시험합니다.
서로 다른 Host ID, 같은 discovery port, UDP discovery/metadata 및 TCP Control 통신 허용이 필요합니다.
localhost의 별도 process 시험은 수행하며 실제 두 장치 LAN 시험은 미수행으로 구분합니다.

### Remote Service 검증 기록

이번 Service 작업에서 실제 실행한 결과이며 이전 Parameter 단계 기록과 구분합니다.

- 관련 C++17 build 및 변경 Network 소스 `-Wall -Wextra -Wpedantic`: PASS, 경고 없음.
- `kcf_control_service_test`: 자동 Service/Application/Element metadata, ID 충돌 방지, 별도 server process 2개 호출: PASS.
- ENDPOINT/HOST/APPLICATION/GROUP/ALL, 16개 동시 Service 요청, 빠른 endpoint 선행 완료 및 timeout 혼합 결과: PASS.
- Parameter와 Service가 peer당 동일 TCP connection을 사용하는 검사: PASS.
- LOCAL exposure, missing capability, request/response type 불일치, incompatible/LOST peer 거부: PASS.
- registration 제거, server SIGKILL, malformed frame/value 거부: PASS.
- 응답 역순/중복/지연, response 중 disconnect의 execution_unknown, reconnect 후 새 요청: PASS.
- 별도 process의 service-owner/service-client CLI 및 client 재시작: PASS.
- 기존 Remote Parameter, Discovery, Compatibility, Network contract 회귀: PASS.
- Service gateway 동작 중 Local IPC 11개 회귀: PASS.
  r41, r5, r4, Topic/Parameter recovery, incomplete Topic/Parameter, legacy Service, integration, Topic Queue, Timer.
- core까지 instrumentation한 Service/Parameter ASan·UBSan 실행: PASS, sanitizer 오류 없음.

로그는 `build/service-local-validation/`, `/tmp/kcf-service-*.log`입니다.
Local API/ABI/storage 및 Local Service protocol 2, Discovery v2를 변경하지 않았습니다.
Control TCP v1은 Service operation만 확장했고 기존 Parameter frame은 유지합니다. 변경된 Network C++ API 사용자는 재빌드해야 합니다.
이 Service 단계 기록 당시 Remote Action은 미구현이었습니다. 현재 Action 구현·검증은 아래에 별도로 기록합니다. 실제 LAN 두 장치 시험은 미수행입니다. 당시 미구현이던 Remote Topic은 아래에 별도 기록하며 authentication/encryption, automatic retry는 아직 미구현입니다.

## Remote Action — 공통 Control과 Local Action 연결

### 자동 metadata와 Local 영향

기존 typed `ActionServer::Create`의 status Topic 이름을 canonical Action 이름으로 사용합니다.
Action 실행 체계를 새로 만들지 않고 **ControlNode → DynamicActionClient → 기존 Local Service/ActionServer**로 연결합니다.
Feedback은 기존 Action status Topic의 최신 snapshot을 읽습니다. Goal/Cancel/Result는 기존 Local Service 경로입니다.

기존 Local Action에는 독립 registry가 없었으므로 최소 관측 metadata를 추가했습니다.
`ActionInfo`, `IntrospectionClient::ListActions`, process별 Action registry(version 1, 최대 16개)를 추가하고,
Runtime registry 시작/종료와 ActionServer 생성/정리, ServiceServer Start/Stop에 등록·노출 훅을 연결합니다.
`ServiceServer::Port()`는 metadata용 읽기 accessor입니다. 사용자 Network 등록 호출은 필요하지 않습니다.
Goal/Feedback/Result에 유효한 `TypeDescriptorTraits`가 있고 Runtime introspection이 활성화되어야 자동 수집됩니다.
관측 등록 실패나 descriptor 부재는 기존 Local Action 실행을 실패시키지 않습니다.

- `metadata.remote_actions`: 정확한 status Topic 이름의 allowlist. 기본 empty는 LOCAL입니다.
- Host/boot/PID/start_ticks + registration으로 owner를 식별하고 기존 Supervisor snapshot에서 Application/Element를 연결합니다.
- Network registration namespace: Parameter/Topic은 원래 ID, Action은 `bit 62 | Local ID`, Service는 `bit 63 | Local ID`.
- Local wrapper의 실제 크기/offset/type/layout identity를 registry에서 수집합니다. descriptor가 없는 기존 Action wrapper의 type=0도 유지합니다.
- **기존 Action API, native Goal/Feedback/Result wrapper 배치, Local Service protocol 2, Topic Format 4, Parameter Format 3은 변경하지 않습니다.**
  별도 관측 registry와 adapter/API를 추가하므로 자동 Action introspection을 사용할 server와 gateway는 재빌드해야 합니다.
  이전 binary의 Local Action은 계속 동작하지만 Action registry를 제공하지 않습니다.
- Topic 복사는 기존 `CopyChannelSequence`의 pin/CAS 보호를 재사용합니다. 이름의 inode, owner PID/start_ticks와 저장 형식을 검사하고 재생성된 SHM에 자동 연결하지 않습니다.
- 실제 Goal 전송 직전에 등록/owner를 재확인합니다. 기존 Service와 같이 registry 확인과 UDP 전송 사이 port 재사용을 원자적으로 막는 신규 handshake는 없습니다.

### API 및 session

```cpp
config.metadata.remote_actions = {"/prepare/status"};
// goal_type/feedback_type/result_type: ParameterWireType(descriptor, ...)
// goal_bytes: EncodeParameterValue(...)로 작성한 canonical field bytes
// node.Start(config);
auto batch = node.StartAction(target, "/prepare/status", goal_type,
                             feedback_type, result_type, goal_bytes).get();
// batch.error 및 모든 endpoint.error/reason/execution_unknown 검사
for (const auto& endpoint : batch.endpoints) {
    if (endpoint.error) continue;
    auto session = endpoint.action; // 호출자가 보관할 thread-safe handle
    auto snapshot = session->Snapshot();
    // accepted, state, feedback_sequence/feedback, terminal/result, error,
    // communication_lost, execution_unknown, cancel_unknown, cancel_accepted
    auto cancel = node.CancelAction(session).get(); // 필요할 때만 명시적 요청
    // cancel 성공은 요청 수락이다. snapshot.terminal을 별도로 확인한다.
}
```

위 future 대기는 Runtime 제어 Loop 밖에서 수행합니다. `Snapshot()`은 callback 없이 최신 상태를 복사합니다.
`ActionBatchResult`는 기존 endpoint별 Control batch의 별칭이며, endpoint별 session과 cancel_unknown을 추가했습니다.
Target은 ENDPOINT/HOST/APPLICATION/GROUP/ALL을 그대로 사용합니다. 선택된 endpoint마다 독립 goal을 생성하고
비동기로 시작합니다. 하나의 거부/timeout이 다른 endpoint의 future dispatch를 직렬화하지 않습니다.
각 Local ActionServer의 **동시에 active goal 하나**라는 기존 계약은 유지합니다. busy 거부는 endpoint별로 반환됩니다.
Local server는 최신 goal/result만 보관하므로 다른 Local/Remote client가 terminal 직후 새 goal을 시작해 이전 result를 교체하면, gateway가 이전 result를 조회하지 못할 수 있습니다. 이때 추적 오류를 알리며 과거 result 저장소를 새로 만들지 않습니다.

`RemoteGoalIdentity = {source Host/boot/PID/start_ticks, discovery session_id, goal_id}`입니다. 일반 request_id와 별도 counter를 사용하며
동일 ControlNode 객체의 Stop/Start 동안 counter를 재사용하지 않습니다. 새 gateway 객체에도 discovery session을 포함하여 이전 객체와 구분합니다. 서로 다른 Host의 같은 숫자 goal_id는 다른 goal입니다.
Gateway는 Local ActionClient와 같은 random 63-bit seed 기반 Local goal ID를 사용하여 기존 Local server에 전달합니다.
Local 숫자 ID의 절대 전역 충돌 방지는 기존 Local protocol처럼 제공하지 않으며 Remote identity는 원문 그대로 별도 추적합니다.
Goal response/Feedback/Result/Cancel은 identity뿐 아니라 원 connection/session, endpoint, 이름, 세 type을 대조합니다.
완료·만료된 request나 goal의 늦은/중복 event는 무시하며 재사용하지 않습니다.

### Wire operation과 검증

기존 Control TCP v1과 peer당 양방향 TCP connection을 공유합니다. Action 전용 socket은 없습니다.

| Operation | Tag | 의미 |
| --- | --- | --- |
| ACTION_GOAL_REQUEST / RESPONSE | 8 / 9 | Goal 제출, 수락 또는 오류 |
| ACTION_FEEDBACK | 10 | 최신 Feedback event |
| ACTION_RESULT | 11 | terminal result 또는 Network 추적 오류 |
| ACTION_CANCEL_REQUEST / RESPONSE | 12 / 13 | 기존 goal의 취소 요청과 수락 여부 |

Action 고정 부분은 **292 bytes**입니다. 기존 172 bytes 뒤에 result TypeIdentity(24), feedback TypeIdentity(24),
goal source Runtime(44), goal_id(8), goal discovery session_id(8), ActionState(1), outcome_unknown(1), reserved(2), feedback_sequence(8)를 넣습니다.
Goal의 TypeIdentity는 기존 type 필드입니다. request_id는 Goal/Cancel request-response용이고 Feedback/Result에서는 0입니다.
`outcome_unknown`은 Local dispatch 후 결과를 확인하지 못한 Goal/Cancel 오류 응답에만 허용합니다.
숫자는 big-endian, payload는 기존 descriptor field codec이며 raw struct/padding은 전송하지 않습니다.
기존 HELLO/Parameter 172-byte 및 Service 196-byte frame은 바뀌지 않습니다. Discovery v2 metadata schema도 유지합니다.

잘못된 operation, identity, state, reserved, 크기, type, direction, payload 길이는 거부합니다.
양쪽에서 ONLINE/peer version/capability/route, provider-consumer 의미, REMOTE scope, 이름과 세 type 전체를 검사합니다.
ControlNode가 실행될 때 REMOTE_ACTION을 추가로 광고하며 기존 DISCOVERY/PARAMETER/SERVICE/5종 Target을 유지합니다.
REMOTE_TOPIC은 광고하지 않습니다. 구버전 peer에 Action operation을 보내지 않습니다.

### Feedback, 취소, 장애 및 자원 상한

- Feedback은 기존 Local status Topic의 **latest** 의미를 유지합니다. 약 10 ms 간격으로 관측하고 sequence가 증가한 값만 전송합니다.
  모든 중간 Feedback 전달은 보장하지 않습니다. client session도 최신 payload 하나만 보관합니다.
- 완료 Result는 terminal state(SUCCEEDED/FAILED/CANCELED)와 result payload를 전달한 뒤 gateway tracking을 제거합니다.
  Local 조회/codec 오류는 `communication_lost/error`로 표시하고 Local Action의 terminal 실패로 추측하지 않습니다.
- Cancel ack의 `cancel_accepted=true`와 실제 `terminal/state=CANCELED`는 별개입니다. 최종 취소 판단·정리는 기존 Application/Local ActionServer 책임입니다.
- **connection loss != Local Action cancel**. peer LOST, TCP reset, Stop, tracking 오류는 Network 자원만 정리합니다.
  Local Action을 자동 Cancel하지 않으며 실제 장비 동작을 임의 중단하지 않습니다.
- Goal request timeout/응답 소실/Local dispatch 불확실성은 `execution_unknown=true`일 수 있습니다.
  Goal 수락 후 응답만 소실될 수 있습니다. 이 경우 추적 누적을 막기 위해 해당 TCP connection을 닫으며,
  같은 connection의 다른 active session도 communication_lost가 될 수 있습니다. Local goal들은 취소하지 않습니다.
- Cancel timeout/응답 소실은 `cancel_unknown=true`일 수 있습니다. 취소 적용 여부는 추측하지 않습니다.
  늦은 ack는 이미 완료된 timeout 결과를 변경하지 않습니다. 수신 가능한 terminal Result는 계속 처리합니다.
- 자동 Goal/Cancel 재전송, Local UDP retry, 자동 resume/re-attach는 없습니다. TCP reconnect는 **새 Goal**만 지원합니다.
  disconnect 전에 완료되었는지 모르는 goal은 Application의 별도 확인·복구 정책이 필요합니다.
- Incoming Action worker는 최대 32개입니다. 별도 32개 Service 작업 슬롯을 Cancel과 공유하여 Goal 모니터가 Cancel admission을 막지 않게 합니다.
  슬롯 부족은 EAGAIN이며 Local callback의 기존 직렬화는 그대로 유지합니다.
- Control pending/outgoing Action admission 상한 256, submission 64, peer connection 64, per-connection TX 256 KiB를 유지합니다.
  worker reply queue 64개에서 Feedback은 48개까지만 허용하고 같은 goal의 미송신 Feedback을 최신값으로 치환합니다.
  TX 상한에서 Feedback은 버릴 수 있습니다. terminal/control 응답을 넣지 못하면 connection을 끊어 실패를 알립니다.
  별도 priority를 만들지 않으며 Parameter/Service/Action 모두 HIGH입니다.
- Result/reject/cancel 완료/LOST/reset/Stop 때 내부 추적을 제거하고 worker를 회수합니다. 사용자 handle의 최종 snapshot은 사용자가 해제할 때까지 유지됩니다.
  Action Local call timeout은 요청 잔여 시간과 200 ms 중 작은 값이며 이후 조회에도 그 상한을 사용합니다.
  Stop은 진행 중 Local call의 bounded timeout을 기다립니다. 기존 metadata/Parameter robust mutex 무기한 대기 한계는 그대로입니다.

### 검증 방법과 범위

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j4
./build/examples/network/kcf_control_action_test
```

테스트는 localhost에서 별도 Local Action owner process들과 서로 다른 HostIdentity를 가진 여러 ControlNode를 실행합니다.
같은 status Topic 이름을 여러 실제 Local owner가 동시에 만들 수 없는 단일 SHM namespace 조건 때문에,
다중 endpoint mixed fan-out은 두 논리 Host가 같은 Local owner를 관측하여 독립 수락/busy 거부를 확인합니다.
별도 Local owner 두 개의 동시 Goal도 확인합니다. 실제 LAN 두 장치 시험은 별도이며 이번 자동 시험으로 대체했다고 표현하지 않습니다.

이 Action 단계 기록에서 Remote Topic은 미구현이었습니다. 현재 Topic 구현은 다음 절을 참고하세요. Authentication/encryption, automatic retry, reconnect 후 기존 Goal re-attach는 여전히 미구현입니다.
Local-only Action·Parameter·Service는 별도 allowlist 없이 원격 실행되지 않습니다.

### Remote Action 검증 기록

이번 Action 단계에서 실제 실행한 결과이며 앞선 Parameter/Service 기록과 구분합니다.

- 전체 Debug C++17 build: PASS. 변경 core 소스의 `-Wall -Wextra -Wpedantic` 검사: 경고 없음.
- `kcf_control_action_test`: 자동 metadata/allowlist/Application identity, Goal 수락·거부·payload, 여러 Feedback와 Result, Cancel: PASS.
- 세 type 불일치, LOCAL scope, missing capability, incompatible/LOST peer, stale owner/start_ticks/registration 거부: PASS.
- 별도 owner의 동시 Goal, 같은 PID/start_ticks 및 같은 숫자 goal_id를 가진 다른 Host 구분, session 불일치 거부: PASS.
- ENDPOINT/HOST/APPLICATION/GROUP/ALL, 복수 endpoint 독립 session과 수락/busy 혼합 결과: PASS.
- Goal timeout/execution_unknown, Cancel timeout/cancel_unknown, 늦은/중복 Feedback·Result·ack: PASS.
- 연결 소실 후 Local Action의 SUCCEEDED를 독립 Local adapter로 확인, 재연결 후 신규 Goal: PASS.
- 잘린/malformed frame·payload, burst Feedback의 latest snapshot, 정상 종료·Stop 정리: PASS.
- 기존 Network 회귀: Remote Parameter, Remote Service, Discovery v2, Compatibility, Network contract PASS.
- 기존 Local IPC 11개 회귀: r41, r5, r4, Topic/Parameter recovery, incomplete Topic/Parameter,
  legacy Service, integration, Topic Queue, Timer PASS. 기존 typed Local Action 정상 완료·Cancel도 PASS.
- core를 포함한 ASan/UBSan: Action·Parameter·Service PASS, sanitizer 메모리/UB 오류 없음.
  병행 빌드가 있던 Service 실행에서 150 ms 독립 응답 timing assertion이 한 번 실패했으며,
  빌드 종료 후 같은 테스트의 단독 실행은 PASS였습니다. 이 기록은 hard real-time 지연 보장이 아닙니다.

로그: `build/action-local-validation/`, `/tmp/kcf-action-*.log`.
실제 LAN 두 장치 시험은 미수행입니다. Application/kcf_tools 코드는 변경하지 않았고 commit/push하지 않았습니다.


## Remote Topic Data Plane

Remote Topic은 IPv4 UDP unicast **NORMAL** 경로입니다. 기존 HIGH TCP Parameter/Service/Action과 socket, worker, queue를 공유하지 않습니다. Discovery multicast에는 payload를 넣지 않습니다. Local Publisher/Subscriber API, Topic Format 4, Parameter Format 3, Local Service protocol 2는 그대로입니다.

### Opt-in과 bootstrap

```cpp
kcf::network::ControlConfig config;
// discovery.host/boot_id/interface_address 등 기존 Gateway 설정 필요
config.topics.enabled = true;              // 기본 false
config.topics.port = 37652;                // 참여 Host가 동일한 UDP port 사용
config.metadata.remote_topics = {"/sensor/sample"}; // 기본 empty
config.topics.max_hz = 50;                 // 기본값; 0.1 ~ 1000 Hz
config.topics.rate_limits = {{"/sensor/sample", 20}};
// 수신 Host의 native layout을 명시. Sample은 Application의 기존 payload type.
config.topics.local_types = {
    {"/sensor/sample", kcf::TypeDescriptorTraits<Sample>::Get()}
};
```

Gateway를 먼저 시작하고 `GetTopicDiagnostics().proxies`에서 준비된 proxy를 확인한 뒤 기존 `Subscriber<Sample>::Create`를 호출합니다. 설정은 Gateway에만 필요하며 Application에 Network 전용 Publisher/Subscriber 등록 API를 추가하지 않습니다. 송신 측 descriptor는 기존 Local introspection에서 자동 수집합니다.

이 순서는 기존 Subscriber가 **이미 존재하는 SHM에만 Open하고 성공 후 endpoint를 등록**하기 때문에 필요합니다. 최초 수신 Host에는 destination-native descriptor catalog가 필요합니다. 이미 존재하는 유효한 Local consumer descriptor도 사용할 수 있지만, 원격 type identity만으로 native offset/layout을 추측하지 않습니다. 빈 proxy 생성은 payload route 생성이 아닙니다. 실제 Subscriber가 없으면 `WAITING_SUBSCRIBER`이며 payload를 전송하지 않습니다.

송신/수신 모두 ONLINE, discovery session, peer compatibility, REMOTE_TOPIC/target capability, REMOTE scope, Publisher/Subscriber role, 정확한 이름과 전체 type identity를 검사합니다. 실제 Local endpoint registration과 PID/start_ticks도 확인합니다. `CheckRouteCompatibility`는 기존 endpoint/Topic compatibility 판정을 재사용합니다. Subscriber metadata 제거/peer LOST 때 route를 제거합니다.

수신자가 실제 Subscriber를 확인하면 source에 bounded `INTEREST`를 200 ms마다 보냅니다. 송신 lease는 750 ms이며 만료 시 송신을 멈춥니다. Endpoint/session이 유지되는 동안 만료 route의 sequence 상태는 bounded slot에 보존하여 늦은 INTEREST로 sequence가 초기화되지 않게 합니다. 이 slot도 `max_routes`에 포함되므로 한도에 도달하면 신규 admission을 거부합니다. Metadata/session 제거 시 회수합니다. 같은 Host의 여러 Local Subscriber는 하나의 proxy/대표 Subscriber route를 공유합니다.

UDP port는 Discovery metadata/TCP port와 별도 설정입니다. 참여 Host마다 같은 numeric Data port를 설정하며 자동 port 협상은 없습니다. 해당 interface/IP당 Gateway 하나를 사용합니다. Data socket bind와 worker 시작에 성공한 ControlNode만 REMOTE_TOPIC을 광고합니다. 기본 비활성 ControlNode는 광고하지 않습니다.

### Latest, rate, wire

Gateway는 기존 `DynamicTopicReader::ReadLatest`로 Local 최신값을 sampling합니다. Local depth/cursor/callback에는 영향을 주지 않습니다. 같은 Local transport sequence를 반복 전송하지 않고, 기본 최대 50 Hz 또는 topic별 rate 제한을 적용합니다. Local 1000 Hz publisher는 계속 동작하며 생략된 sample은 `local_samples_skipped`로 관측합니다. 송신 실패 sample을 재시도하지 않으며 다음 최신 sample로 진행합니다. Poll 기반 soft rate limit으로 hard real-time 주기를 보장하지 않습니다.

Topic wire v1은 magic `0x4b434654` (`KCFT`), **184-byte 고정 header**, 이름과 field payload로 구성합니다. Source/destination Host+Runtime+Endpoint, 양측 discovery session, receiver route ID, 64-bit network sequence, 원본 Local transport sequence, type identity, 길이와 flags(현재 0)를 big-endian으로 encode/decode합니다. Discovery v2/Control TCP v1과 독립된 protocol입니다. Payload는 기존 field codec을 사용하며 native padding을 전송하지 않습니다.

UDP datagram 상한은 **1200 bytes**, wire payload는 **768 bytes**, 이름은 최대 240 bytes입니다. 큰 payload는 `-EMSGSIZE`로 명시적으로 거부하며 truncation/fragmentation/reassembly는 없습니다. IPv4 DF를 설정하므로 경로 MTU가 더 작아 전송할 수 없으면 오류/drop으로 기록합니다. Native descriptor는 최대 16 KiB, alignment 4096까지 지원합니다.

Receiver는 duplicate와 out-of-order/stale packet을 버리고 network sequence gap을 `missed_sequence`로 누적합니다. 손실 후 다음 최신값을 계속 처리하며 retransmission은 없습니다. Local sampling 생략과 UDP sequence gap은 별개입니다. Application 측정 sequence/timestamp/validity는 해석하거나 수정하지 않습니다. `false` boolean payload도 정상 전달합니다.

### Proxy, 충돌, 수명과 상한

Network 전용 private proxy writer는 Local **Format 4, depth=1**과 기존 pin/CAS/robust notification 계약을 사용합니다. 복사 중 reader slot을 덮어쓰지 않습니다. Notify mutex가 busy이거나 안전한 slot이 없으면 `-EAGAIN`으로 해당 수신 sample을 버립니다. Data worker를 막아 Control에 대기를 전파하지 않습니다. Dead-reader pin 자동 회수는 하지 않습니다.

`TopicProxyInfo`는 `is_proxy=true`, Gateway endpoint identity(bit 61 registration namespace), 원래 remote source identity를 제공합니다. Proxy를 Local Application Publisher registry에 등록하거나 Discovery forwarding source로 광고하지 않으므로 echo loop가 생기지 않습니다. `GetTopicDiagnostics()`로 구별할 수 있으며 Tool UI는 변경하지 않았습니다.

동일 이름 Local Publisher/unknown SHM/live Network owner는 `-EEXIST`, 여러 compatible Remote Publisher는 `-EADDRINUSE`로 거부합니다. 이름만으로 source를 병합하지 않습니다. Proxy는 exclusive create하며, 아래 ownership sidecar 검증을 통과한 죽은 Network Gateway의 proxy만 자동 복구합니다. 일반 Local Publisher의 죽은 owner SHM에도 적용하지 않습니다. Name이 다른 inode로 교체되면 기존 writer는 `-ESTALE`을 반환합니다.

Route 제거, peer LOST와 같은 객체의 Stop은 Local SHM 이름을 unlink하지 않습니다. Proxy destructor는 자신의 검증된 Topic/sidecar 이름만 정리합니다. 마지막 값은 남으며 stale 판단은 Application 책임입니다. 같은 DataPlane 객체의 Stop/Start는 보유 proxy mapping을 재사용하지만 discovery session/route는 새로 구성합니다. Gateway process 재시작 시 증명된 stale Network proxy는 자동 재생성합니다. 이전 inode에 연결된 Subscriber는 기존 Local 계약대로 명시적으로 재연결합니다. 일반 Local SHM 자동 정리나 Subscriber mapping 자동 교체는 없습니다. Sidecar 없는 이전 구현의 proxy와 검증 실패 object는 자동 삭제하지 않습니다.

기본 route 상한 64, proxy 상한 32(설정 최대 64), allowlist/type catalog/rate override 각각 최대 64, send/receive socket buffer 요청값 각각 64 KiB입니다(4096~1 MiB 설정 가능, Linux 실제 buffer 크기는 커널 정책에 따름). Application 송신 backlog는 없고 receive latest는 route당 하나입니다. Worker는 한 번에 최대 64 datagram을 처리한 뒤 Local proxy에 최신값을 반영하며 덮어쓴 pending sample은 dropped로 셉니다. History/notice는 각각 최대 64개입니다. Retained proxy도 객체 수명 동안 상한에 포함됩니다.

진단에는 route 상태/reason/error, sent/received/published/dropped, duplicate/out_of_order/missed_sequence/malformed/local_samples_skipped와 전체 stale/resource_drops/socket_errors를 제공합니다. 네트워크 손실/일시적 UDP 오류는 Local Runtime fatal error로 전달하지 않습니다.

### 검증 방법

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j4
./build/examples/network/kcf_topic_data_test
```

테스트는 synthetic payload와 loopback `127.0.0.x`의 다중 process를 사용합니다. 서로 다른 Host의 Local SHM namespace를 재현하기 위해 **테스트 바이너리에서만** 지정된 테스트 Topic 이름의 `shm_open/shm_unlink`를 host별 이름으로 매핑합니다. Production name/API/introspection registry는 변경하지 않습니다. 실제 LAN 장치 시험은 별도이며 이번 자동 검증으로 수행했다고 주장하지 않습니다.

실제 두 장치 수동 시험은 양측 Gateway에 서로 다른 영구 HostIdentity와 올바른 LAN interface, 동일 discovery/data port, 동일 allowlist를 설정합니다. 송신 장치에서 기존 typed Publisher를 실행하고 수신 장치에 맞는 native descriptor catalog로 proxy 준비 후 기존 Subscriber를 실행합니다. Subscriber 제거 시 무전송, 재생성 시 전달, source Gateway Stop 후 timeout/route 제거, 동일 객체 재시작 후 새 session 전달 및 diagnostics를 확인합니다. 별도 장치 실행용 전용 Topic CLI는 추가하지 않았습니다.

Reliable QoS, 인증/암호화, 자동 retry, Action re-attach, Topic fragmentation 및 원격 schema로부터 native descriptor 자동 생성은 미구현입니다.

### Remote Topic 최초 검증 기록 (ownership recovery 추가 전)

이번 Remote Topic 구현 후 실제 실행한 결과입니다. 앞선 Action/Service 및 v5.1 Local Queue 기록과 구분합니다.

- 전체 Debug C++17 build: PASS. 변경 Network 소스 `-Wall -Wextra -Wpedantic`: 경고 없음.
- `kcf_topic_data_test`: 자동 metadata/allowlist/role, Subscriber 없음→무전송, LOCAL/capability/version/type gate, 2/3 peer의 선택적 전달, 기존 Subscriber callback 수신, rate limit: PASS.
- UDP duplicate/out-of-order/gap 이후 정상 수신, malformed/version/size 거부, oversize 명시적 거부, 1500-sequence burst와 막힌 proxy의 bounded/latest/drop, Topic 부하 중 Remote Parameter/Service 완료: PASS.
- Proxy forwarding 제외, 기존 Local Publisher 및 복수 remote source 충돌, LOST/재시작과 stale session 거부, route 제거 후 Local SHM 유지: PASS.
- Format 4 proxy의 pin/CAS 보호와 `-EAGAIN` 시 sequence 유지, robust notify owner death 복구, SHM inode 교체 거부, dead-owner SHM 임의 reclaim 금지, Start/Stop 및 bind 실패 정리: PASS.
- 최종 순차 Network 7개: Topic Data, Remote Action, Remote Parameter, Remote Service, Discovery, Compatibility, Network contract PASS.
- Local IPC 11개: r41, r5, r4, Topic recovery, Parameter recovery, incomplete Topic, incomplete Parameter, legacy Service, integration, Topic Queue, Timer PASS. Topic Queue의 `valid=false` 및 typed Action Success/Cancel도 포함됩니다.
- ASan/UBSan(`detect_leaks=1`, `abort_on_error=1`, `halt_on_error=1`): Topic Data, Remote Action, Remote Parameter PASS. Remote Service는 150 ms timing assertion 실패 후 변경 없는 단독 재실행 PASS. Sanitizer memory/UB 오류 보고는 없었습니다.
- 최초 Network/Local 회귀와 sanitizer build를 병행했을 때 Action의 10초 대기 assertion이 한 차례 실패했습니다. 단독 gdb 실행 및 최종 순차 실행은 PASS였습니다. 실패 원인을 확정하지 않았으며, 시간 제한 테스트의 일회성 실패를 숨기지 않습니다.
- 문서의 로컬 파일 링크 51개 검사: 누락 없음. `git diff --check`: PASS.

로그: `/tmp/kcf-topic-full-build.log`, `/tmp/kcf-topic-network-final.log`,
`build/topic-local-validation/`, `/tmp/kcf-topic-local-queue.log`, `/tmp/kcf-topic-local-timer.log`,
`/tmp/kcf-topic-sanitizer-*.log`. 최초 실패 로그와 단독 재실행 로그도 별도로 남겼습니다.
실제 LAN 두 장치, WAN/NAT 및 hard real-time 검증은 미수행입니다. Application/kcf_tools 및 기존 Local Topic API/storage 구현은 이번 작업에서 수정하지 않았습니다. Commit/push는 수행하지 않았습니다.


## Virtual LAN integration

물리 장치 없이 network/IPC/mount namespace와 독립 `/dev/shm`을 가진 두 Host를 구성하는
별도 [Virtual 2-Host LAN 시험](09_VIRTUAL_LAN_TEST.md)을 추가했습니다.
기존 localhost regression의 SHM 이름 치환을 사용하지 않고 실제 veth multicast/UDP/TCP 경로를 검증합니다.
앞선 단계의 실제 LAN 미검증 기록은 보존하며 Virtual LAN 결과를 물리 LAN 결과로 해석하지 않습니다.
실행 진입점은 `tests/network_virtual/run_virtual_network_test.sh`입니다.


## Network proxy stale SHM recovery

현재 자동 복구 대상은 **완성된 Network ownership sidecar로 증명되는 dead Gateway proxy**뿐입니다. 일반 Local Topic stale 정책이나 Format 4를 확장하지 않습니다. 과거의 정상 proxy에 대한 수동 SHM unlink 요구는 아래 Virtual LAN 재검증에서 해소했습니다. Local Subscriber mapping 재연결은 별개의 기존 계약입니다.

Format 4에는 owner PID/type/layout이 있지만 Network marker, Gateway start_ticks와 source/session이 없습니다. 기존 TypeDescriptor catalog는 type catalog일 뿐 ownership 증명이 아닙니다. 따라서 Network 전용 `/dev/shm/kcf_network_proxy_<topic-hash>` sidecar를 추가했습니다. 이름의 hash/prefix만으로 삭제하지 않습니다.

Sidecar **v1**은 최대 512 bytes의 명시적인 big-endian record입니다. Magic/version/reserved, 전체 logical Topic 이름, proxy registration identity(bit 61), Gateway Host/boot/PID/start_ticks, source Endpoint identity, 양쪽 생성 시점 session, wire type identity, native size/alignment/layout, SHM device/inode/length, checksum을 담습니다. Native struct padding을 저장하지 않습니다. Checksum/hash는 손상 감지와 경로 선택용이며 인증이 아닙니다. 같은 OS 사용자에게 임의의 파일 위조 권한이 있는 공격자를 방어하는 인증 계층은 이번 범위가 아닙니다. Source/session은 생성 당시 provenance이며 remote route 갱신 정보와 구분합니다.

복구 순서:

1. 기존 Local Topic creator가 사용하는 `/dev/shm` directory flock으로 name 생성/복구를 직렬화합니다. Lock 경합 시 비차단 `-EAGAIN`입니다.
2. Topic exclusive create의 `-EEXIST`에서만 기존 Topic 복구 검사를 시작합니다. 기존 fd flock도 확보합니다.
3. Sidecar magic/version/checksum/상한, topic/type/Local Host/boot, Topic inode/device/length, immutable Format 4 header의 owner/type/layout/depth를 모두 검사합니다. UID/regular file/link 상태도 확인합니다.
4. `/proc/<pid>/stat`의 start_ticks가 같으면 LIVE입니다. PID는 같아도 ticks가 다르면 이전 incarnation은 DEAD입니다. `/proc`가 없어도 `kill(pid,0)`의 ESRCH로 재확인해야 DEAD입니다. Permission/read/parse 오류와 불확실성은 UNKNOWN으로 거부합니다. 동일 ticks의 unreaped zombie도 보수적으로 LIVE이며 먼저 reap해야 합니다.
5. 검증된 DEAD만 stale로 표시하고 named inode를 다시 확인합니다. **Topic 먼저, sidecar 다음**으로 unlink 후 새 pair를 생성합니다. 정상 source peer가 ONLINE이어도 Local Gateway owner가 DEAD이면 복구할 수 있습니다.
6. Cleanup 중 Topic만 삭제된 채 죽으면 다음 생성에서 남은 완전한 sidecar의 topic/type/host/boot와 dead owner를 검증하여 orphan record를 정리합니다. Live/불확실/손상 record는 거부합니다.

일반 Local Publisher가 metadata에 존재하면 복구 검사를 하지 않고 conflict를 반환합니다. Sidecar가 없거나 Topic와 관계가 맞지 않는 object, 다른 type, live Network owner는 삭제하지 않습니다. 생성이 끝나기 전의 crash로 sidecar가 없거나 불완전한 경우에도 fail-closed로 남깁니다. 이 경우와 업그레이드 전 marker 없는 proxy는 운영자의 명시적 확인·정리가 필요합니다. Unknown object를 자동 채택하거나 마이그레이션하지 않습니다.

같은 객체의 Stop/Start는 기존 reader를 불필요하게 끊지 않도록 proxy mapping을 유지합니다. 객체 정상 소멸은 자신이 소유한 동일 record/inode/header만 정리합니다. SIGKILL에서는 hook이 실행되지 않으므로 다음 Gateway의 독립적인 ownership recovery가 필요합니다. 이미 매핑된 reader의 메모리를 in-place 초기화하거나 강제로 unmap하지 않습니다. 새 Gateway/Topic 생성 뒤 기존 Subscriber를 Close/Create 또는 process 재시작으로 재연결해야 합니다.

진단은 `TopicProxyInfo.recovery/stale_detected`, `TopicRouteInfo.proxy_recovery`로 제공합니다. `CREATED`, `LIVE_NETWORK_OWNER`, `LOCAL_PUBLISHER`, `UNKNOWN_OWNERSHIP`, `STALE_DETECTED`, `RECOVERED`, `VALIDATION_FAILED`, `CLEANUP_FAILED`를 구분합니다. Cleanup API도 failure code/state를 반환합니다.

Topic Format **4**, Parameter Format **3**, Local Service protocol **2**, Discovery **v2**, Control TCP **v1**, Topic wire **v1**은 변경하지 않았습니다. 추가 영향은 Network 전용 sidecar v1과 process-local diagnostic 필드이며 해당 C++ 사용자는 재빌드합니다.

실제 검증 결과는 [Virtual LAN 재검증](09_VIRTUAL_LAN_TEST.md#network-proxy-자동-복구-재검증)에 별도로 기록합니다. 물리 LAN 검증으로 해석하지 않습니다.

### Proxy recovery 검증 기록 (2026-09-29)

- 전체 Debug C++17 build 및 새 Network 코드 `-Wall -Wextra -Wpedantic`: PASS, 경고 없음.
- `kcf_proxy_recovery_test`: 정상 소멸/재생성, SIGKILL 잔존 proxy 복구·실제 payload 읽기, PID 동일/start_ticks 불일치 및 UNKNOWN 판정, live Gateway 거부, 살아 있거나 죽은 Local Publisher 보호, unknown/corrupt/type/sidecar inode mismatch, 실제 Local object로의 name 교체 보호, invalid caller의 무변경, orphan sidecar 중간 상태, concurrent recovery의 단일 winner: PASS.
- Sidecar codec은 전체 truncation과 각 byte 손상 거부를 검사했습니다. Record는 최대 512 bytes로 제한합니다.
- 기존 Network 7개: Remote Topic, contract, compatibility, Discovery, Parameter, Service PASS. Action은 최초 10초 대기 assertion 실패 후 변경 없는 단독 gdb 실행에서 PASS였습니다. 최초 Topic conflict 대기 역시 한 차례 실패했지만 단독·최종 회귀와 sanitizer 실행에서 PASS했습니다. 원인을 확정하지 않았으며 timing 실패 이력을 숨기지 않습니다.
- Local IPC 11개: r41, r5, r4, Topic/Parameter recovery, incomplete Topic/Parameter, legacy Service, integration, Topic Queue, Timer PASS.
- ASan/UBSan: proxy recovery와 전체 Remote Topic 테스트 PASS (`detect_leaks=1`, `abort_on_error=1`, `halt_on_error=1`). Memory/UB 오류 보고 없음.
- Virtual LAN 전체 재검증: PASS. 수동 shm unlink 없이 수신 Gateway SIGKILL → ownership recovery → 새로운 Subscriber 연결 → 수신 재개, 정상 Gateway 종료·재시작도 PASS.
- Bash/Python 문법, 문서 링크 및 `git diff --check`: PASS.

로그는 `build/proxy-recovery-validation/`, `build/proxy-local-validation/`, `build/network-virtual/proxy-recovery/`, `/tmp/kcf-proxy-action-diagnosis.log`에 있습니다. 실제 물리 LAN은 미검증입니다. Commit/push 및 Application/kcf_tools 변경은 하지 않았습니다.
