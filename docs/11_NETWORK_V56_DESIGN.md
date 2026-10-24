# 11. KCF v5.6 — Framework-managed Network Architecture Contract

작성: 2026-10-23. NET56-1 설계 문서이며 **현재 구현 버전은 5.5.0**이다.
이 문서의 v5.6 목표·권장안은 구현 완료나 검증 PASS를 의미하지 않는다.
C++ API, CMake, Wire 및 Local SHM은 이번 단계에서 변경하지 않는다.

## 1. 기준과 현재 구현 구분

대조 기준은 [Network Architecture](08_NETWORK_ARCHITECTURE.md),
[Deployment Policy](10_NETWORK_DEPLOYMENT_POLICY.md), 아래 공개 계약과 지정 구현이다.

- [PeerDiscovery](../kcf/include/kcf/network/peer_discovery.hpp) / [구현](../kcf/src/network/peer_discovery.cpp)
- [Local Metadata](../kcf/include/kcf/network/local_metadata.hpp) / [구현](../kcf/src/network/local_metadata.cpp)
- [ControlNode](../kcf/include/kcf/network/control_node.hpp) / [구현](../kcf/src/network/control_node.cpp)
- [TopicDataPlane](../kcf/include/kcf/network/topic_data_plane.hpp) / [구현](../kcf/src/network/topic_data_plane.cpp)
- [RemoteControlClient](../kcf/include/kcf/network/remote_control_client.hpp),
  [RemoteTopicClient](../kcf/include/kcf/network/remote_topic_client.hpp),
  [Diagnostics Client](../kcf/include/kcf/network/network_diagnostics_client.hpp)
- [Identity 모델](../kcf/include/kcf/network/types.hpp),
  [Compatibility](../kcf/include/kcf/network/compatibility.hpp),
  [Descriptor/Canonical codec](../kcf/include/kcf/network/parameter_codec.hpp)

08 문서의 상단 v5.4 소개와 단계별 미구현 목록은 현행 구현 전체를 대체하지 않는다.
10 문서의 v5.3/v5.4 구현 대조와 물리 LAN 기록도 당시 기록으로 보존한다.

| 항목 | 현재 v5.5 코드 기준 |
| --- | --- |
| Discovery/Metadata | IPv4 multicast announce, 필요 시 unicast metadata, ONLINE/LOST, 세대 검증 구현 |
| Local introspection | Parameter/Topic/Service/Action 및 Application/Element 매핑 자동 수집 구현 |
| Remote Control | Parameter Get/Set, Service, Action Goal/Feedback/Result/Cancel 구현 |
| Generic schema/Topic | `QueryTypeDescriptor`, Scalar/Fixed Array canonical codec, 정확한 Endpoint 구독 구현. 과거의 schema 교환 미구현 표기는 현재에 적용하지 않음 |
| Diagnostics | opt-in Snapshot/Client 및 누적 Counter 구현 |
| 권한 | 개별 Client allowlist 존재. 과거의 권한 미지원 표현과 달리 접근 정책은 있으나 암호학적 인증·암호화는 없음 |
| 미구현 배포 기능 | 영구 Host ID 자동 발급·저장, 자동 NIC 선택/전환, 공통 서비스 자동 운영, 설정 로더·Debian 설치 정책 |
| 계속 미지원 | Reliable Topic QoS, 변경 요청 자동 retry, Action reattach, WAN/NAT traversal, hard real-time 도착 보장 |

Framework-managed는 **Host의 Network 서비스 수명·설정·정책을 Framework 배포 계층이 관리**한다는 뜻이다.
Application Supervisor의 Managed 자식 목록이나 Local Runtime의 실행 소유권을 확장하지 않는다.

## 2. Host Identity

### 현재 구현

`HostIdentity`는 호출자가 공급하는 nonzero 128-bit 값이다. Boot ID와 별개이며,
`RemoteRuntimeIdentity = Host + Boot + PID + process_start_ticks`,
`RemoteEndpointIdentity = Runtime + registration_id`를 유지한다.
Discovery Start마다 random 64-bit Session을 만든다. 자기 Host ID 패킷은 무시하고,
Peer Table은 Host ID 하나당 한 인스턴스를 전제한다. 장치 전체 singleton/provisioning은 없다.
다른 Boot/Session 또는 주소의 announce는 기존 peer timeout 전에는 채택하지 않는다.
이 동작은 중복 ID 인증·복구 기능이 아니다.

### v5.6 목표 — 권장안

- Host당 `/var/lib/kcf/host-id`에 영구 ID 하나를 보존한다. 최초 사용 시 OS random으로 생성하고
  잠금·원자적 설치·파일 및 디렉터리 동기화 후 사용한다. 동시에 설치/시작해도 한 ID만 확정한다.
- 파일 누락일 때만 자동 생성한다. 손상·권한 오류·쓰기 실패는 명시적 시작 오류이며 매번 임시 ID로 우회하지 않는다.
  재설치/업그레이드는 ID를 보존한다. 삭제·재발급은 관리자 명시 작업이다.
- Boot는 Linux boot_id, Session은 서비스 Discovery 시작 세대, Runtime/Endpoint는 실제 등록 세대를 유지한다.
  DHCP 주소는 Identity가 아니다. Gateway 재시작은 Local Runtime의 재시작을 뜻하지 않는다.
- 이미지 제작 시 host-id를 제외한다. 복제 완료 장치는 네트워크 활성화 전 provisioning한다.
  복제된 machine-id/MAC만으로 고유성을 보장하거나 이를 Host ID로 직접 사용하지 않는다.
- Host 서비스는 OS lock으로 중복 실행을 막는다. PID 파일만으로 판단하지 않으며 서비스/수동 실행이 같은 lock을 사용한다.
- 동일 ID의 상충 Boot/Session/주소 관측은 **충돌 의심**으로 기록한다. 지연 패킷·재시작과 구별할 관측 창을 둔다.
  확인 전 신규 원격 제어를 보수적으로 중단하고 운영자에게 알리되 Local IPC는 유지한다.
  원격 패킷만으로 영구 ID를 변경하지 않는다. 관리자 현장 확인 후 복제 장치 한쪽만 정지·재발급하고 권한/선택을 재설정한다.
  비인증 패킷으로 충돌 경고/차단을 유발할 수 있는 DoS 한계도 명시한다.

### 변경 대상

신규 Host ID 저장소/provisioning 및 서비스 singleton 계층. Discovery의 자기 ID 무시 전 충돌 관측 경로 검토.
기존 Identity 구조체는 유지하며 Gateway와 Tools의 공유 방식은 4절을 따른다.

### 호환성 위험

기존 수동 Host ID에서 새 ID로 이동하면 allowlist와 저장된 대상 선택이 달라진다.
관리자 승인으로 기존 ID를 가져오는 migration을 제공하고 몰래 교체하지 않는다.
같은 Host ID로 기존 Gateway와 독립 Tool Discovery를 함께 시작하는 것은 여전히 금지한다.

### 검증 기준

동시 최초 시작, crash 중 파일 쓰기, 읽기 전용/손상 저장소, 재부팅/재설치, 이미지 복제,
동일 ID 중복 실행, 오래된 세대 패킷 및 위조 충돌 패킷 시험.
영구 ID 불변·단일 서비스·Local Runtime 지속 실행을 확인한다.

대안: 각 프로세스가 임의 ID 생성은 구현은 쉽지만 Host 지속성과 권한 설정을 깨므로 기본 경로에서 제외한다.
충돌 관측 창과 관리자 재발급 도구의 상세 UX는 미확정이다.

## 3. Network Discovery와 NIC 선택

### 현재 구현

`DiscoveryConfig::interface_address` 기본은 `0.0.0.0`이며 OS 경로 선택에 의존할 수 있다.
명시적 NIC 탐색·DHCP 감시·failover 기능은 아니다. Multicast `239.255.75.67:37651`, TTL=1,
announce 1초, peer timeout 4초, metadata retry 200ms가 기본이다.
Metadata revision 변경 시 bounded snapshot을 교환하며 payload는 announce에 넣지 않는다.

### v5.6 목표 — 권장안

- 서비스가 단일 IPv4 NIC를 선택한다. 관리자 인터페이스 지정이 최우선이고, 기본은 UP/RUNNING,
  multicast 가능, non-loopback IPv4 후보 중 유일한 우선 경로를 선택한다.
  route metric 등을 적용해도 모호하면 임의의 첫 NIC가 아닌 `설정 필요` 상태를 보고한다.
- VPN/tunnel/관리망의 자동 노출을 피하도록 후보 정책을 검증한다. 다중 NIC 동시 광고/자동 bridging은 초기 범위 밖이다.
  명시한 NIC가 없어지면 무관한 NIC로 자동 전환하지 않는다. Loopback은 시험용 명시 설정으로 허용한다.
- 주소/link/route 변경을 감시하고 debounce 후 Network 인스턴스를 정리·재바인딩한다.
  Host/Boot는 유지하고 새 Discovery Session을 사용한다. NIC가 없으면 서비스는 네트워크 대기 상태로 남고 Local IPC에는 관여하지 않는다.
- 재시작 폭주를 막는 bounded backoff와 관리 상태를 제공한다. 원격 peer의 기존 timeout/퇴역 Session 보호를 생략하지 않는다.
  주소 복구와 Endpoint 재발견은 허용하지만 이전 Action/변경 요청 재전송이나 기존 Topic handle의 자동 세대 전환은 금지한다.
- 일반 사용자는 Host ID/IP를 입력하지 않는다. 명시 설정은 시험·특수 망용 override로 남긴다.

### 변경 대상

신규 NIC resolver/주소 감시 및 서비스 상태 기계. `peer_discovery.cpp`의 socket lifecycle 재사용.
초기 구현은 전체 Network 인스턴스 재생성으로 변경 경계를 명확히 한다.

### 호환성 위험

Session 교체와 peer timeout 때문에 재발견 공백이 발생한다. 무중단 DHCP 이동을 약속하지 않는다.
VLAN, Wi-Fi isolation, multicast filtering, VM bridge/UFW, 다중 NIC는 별도 검증이 필요하다.

### 검증 기준

무설정 단일 NIC, DHCP IP 교체, link down/up, NIC 제거, 동률 다중 경로, 명시 NIC 부재,
multicast 차단·복구에서 주소/세대/소켓 정리와 stale 요청 거부를 확인한다.
기존 Discovery 간헐적 준비 Timeout 이력은 새 시험 결과와 분리한다.

대안: 모든 NIC 광고는 빠른 접근성을 주지만 현재 단일 주소 Peer Table 및 같은 Host 중복 방어와 충돌한다.
멀티홈 wire/model 설계 없이 적용하지 않는다.

## 4. Host 공통 Gateway와 Tools

### 현재 구현

`ControlNode`는 호출자가 Start/Stop하는 Host 단위 라이브러리다. 별도 Network worker로 Local IPC와 분리되어 있다.
상시 서비스 실행 파일/자동 운영 계약은 아직 없다. Control TCP와 Metadata UDP는 같은 숫자의 별도 포트를 사용하며
기본 0은 OS 할당이다. Topic은 기본 OFF, 기존 Gateway 포트는 37652이다.
기존 Tools용 RemoteControl/Diagnostics/RemoteTopic Client는 하나의 **observer 프로필** PeerDiscovery를 공유한다.
`ControlNode` 내부 Discovery를 외부 프로세스에서 빌려 쓰는 API나 로컬 서비스 RPC는 없다.

### v5.6 목표 — 권장안

- Application과 별도 Host 서비스 프로세스 하나가 Discovery/Control/Topic, Metadata 및 정책을 소유한다.
  OS 서비스 관리자가 부팅/명시적 enable 시 시작하고 종료·재시작을 관리한다. Application/GUI가 각자 Gateway를 fork하지 않는다.
- 서비스 crash에는 제한된 backoff/시작 횟수 제한을 적용하고, 설정·ID 충돌은 반복 재시작 대신 실패 상태로 남긴다.
  이 정책은 **Network 서비스에만 해당**한다. Supervisor의 수동 Reset/Shutdown과 Managed 프로세스의 자동 재시작 금지는 유지한다.
- 배포 권장 포트는 Discovery UDP 37651, Control TCP/Metadata UDP 39001, Topic UDP 37652이다.
  39001은 아직 라이브러리 기본값이 아니다. 서비스 설정에서 적용하고 Host별 override를 검증한다.
- Tools 기본 경로는 권한 있는 로컬 Unix-domain 연결을 통해 서비스의 Peer/Metadata/Diagnostics/Remote 작업에 접근한다.
  UID/GID 검증, bounded 요청/응답·구독, client disconnect 정리, 서비스 세대 토큰을 갖춘 별도 로컬 facade를 설계한다.
  이는 **신규 제안이며 현재 공개 API가 아니다**. 새 LAN codec을 만들지 않고 기존 실행 경로를 재사용한다.
- Gateway 프로필에 observer 전용 Client 클래스를 그대로 연결할 수 있다고 가정하지 않는다.
  서비스 내부 facade는 ControlNode의 실행 경로와 정확한 Endpoint Topic 경로를 연결하며,
  Descriptor/Diagnostics 노출 및 권한 적용을 별도로 검증한다. 같은 Host의 논리 Tool 사용자 권한은 로컬 admission으로 제한한다.
  원격에서는 서비스 Host를 보게 되므로 사용자별 인증을 제공한다고 주장하지 않는다.
- Tools v0.14의 기존 독립 Observer 모드는 유지한다. 서비스 영구 Host ID를 재사용하지 않고
  별도의 논리 Observer ID와 동적 Metadata/Topic 수신 포트를 사용한다. 서버의 별도 allowlist가 필요하다.
  기본 서비스 경로와 호환 Observer 경로를 구별하며 자동으로 권한이 넓은 경로로 fallback하지 않는다.
- 서비스 Stop은 admission 중단, pending 결과 확정/불확실성 표시, worker/socket/소유 proxy 정리를 수행한다.
  Action 취소·완료를 보장하지 않고 기존 handle은 재시작 후 무효화한다. 일반 Application의 SHM/PID를 삭제·종료하지 않는다.

### 변경 대상

신규 서비스 실행 계층/설정/로컬 facade와 향후 Tools adapter. 기존 ControlNode/TopicDataPlane의 필요한 내부 통합 지점만 확장한다.
설치·service unit·계정/권한·업그레이드는 별도 배포 단계이다.

### 호환성 위험

프로세스 간 C++ 객체 공유는 불가능하다. facade는 별도 로컬 API 버전·수명 계약이 필요하다.
기존 RemoteControlClient의 Action 연결 상한, Diagnostics/Control 연결 제한을 무제한으로 늘리지 않는다.
살아 있는 Local process가 robust mutex를 잡고 멈추면 기존 backend/Stop join도 지연될 수 있다.
서비스 관리자의 최종 프로세스 종료 경계는 필요하지만 이를 Local mutex recovery 보장으로 설명하지 않는다.

### 검증 기준

Gateway 없는/있는 Host에서 Tool 연결, 두 Tool 동시 접속, 독립 Observer 공존,
서비스 crash/재시작 중 Local Topic/Runtime 지속 실행, 변경 요청 execution_unknown,
Action Cancel ACK와 terminal 구분, Topic 명시 재선택, shutdown 자원 회수를 검증한다.
제한된 재시작 소진과 Local mutex 정지 시 관리 프로세스의 종료 경계도 시험한다.

대안: Gateway와 Tools에 같은 ID의 Discovery를 각각 만드는 방식은 현재 계약과 충돌하므로 채택하지 않는다.
별도 논리 ID Observer만 계속 쓰는 방식은 호환성에는 유리하지만 장치 단일 서비스 목표를 충족하지 못한다.

## 5. Endpoint 자동 발견과 접근 정책

### 현재 구현

`CollectLocalMetadata`는 유효한 Descriptor의 Runtime/Endpoint를 수집하고 Supervisor PID/start_ticks로 Application을 매핑한다.
`runtime_pids`가 비어 있으면 전체 살아 있는 Local Runtime이 대상이다. 현재 `remote_topics/parameters/services/actions`는
정확한 이름 목록으로 REMOTE를 지정한다. Discovery metadata에는 LOCAL 정보도 포함될 수 있으므로
**LOCAL scope를 metadata 비공개와 동일시하지 않는다**.
Descriptor/Get/Set/Service/Goal/Cancel Client allowlist는 각각 기본 빈 목록이고 Diagnostics는 별도 opt-in이다.
Gateway 간 경로의 기존 정책과 observer Client 권한 경로는 동일하지 않다.
Topic proxy는 forwarding source로 재광고하지 않는다.

### v5.6 목표 — 권장안

- Local introspection을 재사용하고 Application별 Gateway 코드·컴파일된 Topic 목록을 제거한다.
  Host 설정의 선언적 정책으로 Application/Element/kind/role/name/type을 선택하여 노출한다.
  초기 지원은 정확한 selector와 명시 규칙을 우선하며 자동 발견을 자동 공개로 바꾸지 않는다.
- 정책은 (1) metadata/schema 공개, (2) Topic 송수신 허용, (3) Get/Set/Service/Goal/Cancel 실행 허용을 분리한다.
  원격 제어 기본 거부, 불명확/충돌 규칙은 거부 우선이다. Diagnostics도 opt-in을 유지한다.
  Gateway-to-Gateway와 독립 Observer 모두 실행 직전 정책을 검사한다.
- 정책의 이름은 관리용 selector이며 dispatch는 현세대의 정확한 Endpoint/expected_session/전체 TypeIdentity로 고정한다.
  동일 이름의 다른 Application/Host에 권한이 퍼지지 않도록 selector 범위를 명시한다.
- 자동 발견된 Descriptor로 codec/catalog를 구성하되 기존 scalar/fixed-array·상한·owner 재검증을 유지한다.
  지원되지 않는 타입은 명확히 제외하고 native memory를 전송하지 않는다.
- 정책 업데이트는 검증 후 원자적으로 적용한다. 철회된 endpoint의 신규 요청/Interest를 막고 route를 정리한다.
  이미 실행된 Service/Action의 rollback/취소는 보장하지 않는다. 설정 reload 가능 범위는 후속 명세에서 확정한다.
- Host ID/allowlist는 인증이 아니다. 신뢰 LAN/NIC/subnet과 로컬 서비스 접근권한을 함께 제한한다.
  인터넷 노출·자동 방화벽 개방은 하지 않는다.

### 변경 대상

Host 정책 resolver 및 `local_metadata.cpp`, `control_node.cpp`, `topic_data_plane.cpp`의 노출/dispatch gate.
Local Endpoint 등록 API/SHM 변경 없이 설정과 기존 introspection 필드로 시작한다.

### 호환성 위험

현재 이름 단위 allowlist를 Application별 정책으로 변환할 때 같은 이름의 여러 Runtime이 확장 허용되지 않아야 한다.
기존 명시 설정 API는 호환 경로로 보존하며 보안 기본값을 넓히지 않는다.
Metadata 비공개 필터가 새로 적용되면 기존 Graph에서 보이는 범위가 줄어들 수 있다.

### 검증 기준

신규 Application 자동 발견/제거, LOCAL-only 및 비공개 metadata, default deny, 작업별 독립 허용,
동일 이름 다중 Runtime, 세대 변경, 정책 철회, unsupported descriptor, proxy 재광고 방지를 검증한다.

대안: 모든 발견 endpoint를 REMOTE로 바꾸는 방식은 설정이 간단하지만 기본 거부와 충돌하므로 제외한다.

## 6. Topic QoS / Rate / Freshness

### 현재 구현

`TopicConfig::max_hz=50`은 **송신 route별 최신값 sampling 최대율의 기본값**이다.
전체 Host 대역폭 상한이나 Local Publish/Callback 주기 또는 최소 전달률이 아니다.
`rate_limits`의 이름별 값은 기본값을 대체하며 현재 허용 범위는 0.1~1000Hz이다.
`Transmit()`은 활성 Interest route에서 `ReadLatest`하고 같은 Local sequence는 재송신하지 않는다.
Soft poll 기반이므로 50Hz 설정이 실제 50Hz 도착을 보장하지 않는다.
Local Depth/KEEP_LAST와 무관하게 Network는 latest-only이고 payload 상한은 768 bytes이다.
Generic Client는 INTEREST의 UDP source port로 DATA를 받아 Gateway와 다른 동적 포트에서 공존한다.

### v5.6 목표 — 권장안

아래는 **제안 정책 필드**이며 현행 C++ API 이름이 아니다.

| 정책 항목 | 의미 |
| --- | --- |
| 전송 목표율 / 최대율 | Topic provider의 목표 sampling Hz와 허용 상한. Source 실제 발행보다 많은 새 값을 만들지 않음 |
| 요구율 | Consumer가 요구하는 최소 전송 목표. Admission 적합성 조건이지 실제 수신 보장 아님 |
| Deadline | 수신 측 새 sample 간 최대 허용 공백. 수신 Host monotonic clock으로 violation 관측 |
| Freshness | 마지막 수신 이후 허용 age 및 필요 시 Application 측 측정 age 정책. Deadline과 별도 |
| 전달 방식 | 초기 v5.6도 best-effort/latest-only. Reliable history/retransmission은 별도 범위 |

- 요구가 없는 기존 telemetry만 기본 50Hz를 상속한다. 100Hz 명령은 목표·상한을 명시하고
  50Hz로 설정된 경로는 admission 실패/불충족 상태로 표시한다. 조용한 clamp는 금지한다.
  QoS 요구를 이해하지 못하는 구 peer는 엄격한 요구 경로에서 unsupported로 처리한다.
- Topic별 실제 Endpoint/type에 정책을 결합하고, 요구율 > 허용 상한, 지원 범위 초과,
  자원 부족은 명시 거부한다. 승인 이후 실제 수신률 미달도 별도로 관측한다.
- 전송 목표, 성공 send, receive, proxy publish, Application 소비율을 구분한다.
  `local_samples_skipped`는 최신값 sampling 생략이다. `missed_sequence`는 transport gap으로
  송신 오류도 포함할 수 있으므로 순수 네트워크 packet loss로 단정하지 않는다.
  pending overwrite/duplicate/out_of_order와 구별하고 카운터는 route/session 경계에서 해석한다.
- 수신 시각 기반 age는 네트워크 지연 전체나 원본 측정 age를 증명하지 않는다.
  서로 다른 Host의 CLOCK_MONOTONIC 값을 빼지 않는다. 종단 age가 필요하면 별도 clock sync·timestamp 의미·오차 예산이 필요하다.
- Payload timestamp/측정 sequence/valid는 Application 책임이다. Framework가 payload valid를 변경하지 않는다.
  장치 측 Application은 새 명령 수신 공백과 신뢰 가능한 freshness를 검사해 자체 timeout/fail-safe를 수행한다.
  Discovery의 4초 timeout을 모터 명령 안전 timeout으로 사용하지 않는다.
- LAN 혼잡/손실/OS 지연에서 hard real-time 도착 보장은 없다. 경고·불충족 보고와 장치 안전 동작은 별개이다.

### 변경 대상

정책/QoS admission, Topic rate 선택 및 진단 확장. Local Publisher/Subscriber/ProcessRuntime 주기는 변경하지 않는다.
수신 요구·capability 협상은 7절의 후속 계약을 먼저 확정한다.

### 호환성 위험

이름 단위 rate override는 같은 이름의 다른 provider를 구별하지 못한다. Endpoint별 선택을 추가하면서 기존 설정은 유지한다.
기존 Counter 의미를 바꾸거나 Local Queue를 Network reliable queue로 해석하지 않는다.

### 검증 기준

200Hz Local/기본 50Hz sampling, 명시 100Hz 요구 승인·불충족 거부, 같은 이름 서로 다른 정책,
복수 consumer 부하, loss/jitter/deadline/freshness, 장치 timeout, Control/Diagnostics 동시성,
구 peer의 unsupported 및 순수 packet loss와 sampling gap 구분을 검증한다.

대안: 기본값 전체를 100Hz 이상으로 올리는 방식은 자원 부하만 바꾸고 개별 명령 계약을 해결하지 못하므로 채택하지 않는다.

## 7. 호환성, Wire와 Tools v0.14

### 현재 구현

FrameworkVersion은 5.5.0, NetworkProtocol은 1.0, Discovery wire는 v2,
Control TCP wire는 v1, Topic DATA/INTEREST wire는 v1이다.
Endpoint/control-model schema 버전과 위 버전은 별개다. Topic SHM 4 / Parameter SHM 3 등
기존 Local storage/protocol을 이 문서에서 변경하지 않는다.
`NETWORK_PROTOCOL`은 Network major 및 필요한 capability/type 계약을 검사한다.
`EXACT_FRAMEWORK_VERSION`은 Framework 전체 버전 일치를 요구하므로 향후 5.5↔5.6은 불일치한다.
Discovery v2는 임의 trailing extension을 수용하지 않으며 capability만 추가해 body를 늘릴 수 없다.

### v5.6 목표 — 권장안

| 확장 | 권장 호환 경계 |
| --- | --- |
| Host ID 저장/NIC/서비스 운영 | LAN Wire 변경 불필요. 기존 Host/Boot/Session 필드 사용 |
| 정책/자동 catalog | Local resolver 우선. 기존 Metadata 필드와 접근 gate 재사용 |
| Tools 로컬 facade | 별도 로컬 protocol/API 버전 협상 필요. LAN 또는 Local SHM ABI 버전으로 혼용하지 않음 |
| QoS 요구/응답 | 기존 bounded Control frame에 opt-in operation/capability로 최소 확장하는 안 권장. 구 peer에는 전송하지 않음 |
| Topic port 자동 전달 | 초기 서비스 공통 37652 유지. Host별 Publisher 포트 자동 협상이 필요하면 QoS/route 정보 확장과 함께 명시 설계 |
| DATA timestamp/새 delivery 의미 | 현재 DATA v1에 몰래 추가하지 않음. 초기 v5.6에서는 수신 clock 기반 관측으로 제한 |

QoS capability bit·operation tag·canonical payload·오류 값은 아직 할당하지 않았다.
Endpoint/양쪽 Session/TypeIdentity에 묶인 유한 수명 admission을 권장하며, 새 Session에 재사용하지 않는다.
QoS 승인과 기존 INTEREST의 결합·철회·만료 규칙을 구현 전에 명세하고 codec negative test를 먼저 작성한다.
이 확장으로 Network minor 또는 Control envelope 갱신이 필요한지는 최종 field/layout 검토에서 결정한다.
**NET56-1에서 새로운 Wire 버전이나 하위 호환성을 확정·선언하지 않는다.**

### 변경 대상

호환성/Capability·Control codec·Topic admission의 제한된 확장과 신규 local facade.
이후 별도 Tools adapter 작업에서 서비스 선택, 실제 적용 정책·불충족 표시 및 세대 무효화를 연동한다.
Tools v0.14를 서비스 facade/QoS 신기능을 이미 지원하는 버전으로 기술하지 않는다.
기존 독립 Observer의 Diagnostics/Control/Generic Topic 기능은 legacy 경로로 계속 검증한다.

### 호환성 위험

- 서비스가 원격에서 Gateway 프로필로 보일 때 기존 observer 전용 권한을 그대로 적용할 수 없다.
  Gateway 간 요청에도 정책을 적용하고, 로컬 Tools 권한이 다른 사용자에게 전이되지 않게 한다.
- 새 QoS가 없는 v5.5/Tools v0.14와 legacy telemetry는 계속 통신할 수 있어야 한다.
  엄격한 요구가 있는 명령 경로에서는 legacy fallback으로 성공을 가장하지 않는다.
- 서비스 변경 후 기존 Action은 reattach하지 않고 변경 요청은 자동 retry하지 않는다.
  `execution_unknown`/`cancel_unknown`을 로컬 facade에서도 잃지 않는다.

### 검증 기준

v5.5↔v5.6/Tools v0.14 호환 matrix, capability 미광고·알 수 없는 bit/tag,
잘못된 길이·세대·TypeIdentity, old DATA/INTEREST 지연 수신, 정확한 Endpoint 선택을 시험한다.
기존 Parameter/Service/Action/Topic/Descriptor/Diagnostics와 Network/Local IPC 회귀를 보존한다.
실패 후 재실행 PASS는 최초 FAIL과 별도로 남긴다.

## 8. 후속 구현 순서와 Rollback 경계

아래 번호는 NET56-1 이후의 **작업 분할 제안**이다. 단계 완료 전 기본 경로를 다음 단계로 전환하지 않는다.

| 단계 | 작업 | 통과 조건 | Rollback 경계 |
| --- | --- | --- | --- |
| NET56-2 | 영구 ID/provisioning, singleton lock | 원자적 생성·중복·복제·손상 시험 | 기존 명시 ID 설정으로 복귀. 생성 ID는 삭제하지 않고 보존 |
| NET56-3 | NIC 선택/주소 감시/세대 수명 | DHCP/link/multicast/다중 NIC·자원 시험 | 명시 NIC와 수동 Network Start/Stop. pending 요청 replay 없음 |
| NET56-4 | Host 서비스/설정/Endpoint 정책 | default deny, 자동 수집, Local 장애 격리, 제한된 서비스 restart | 서비스를 중지·포트/lock 회수 후 기존 Gateway 실행. 두 인스턴스 동시 운영 금지 |
| NET56-5 | 로컬 facade 및 기존 실행 경로 통합 | 다중 Tools, Control/Action/Topic/Diagnostics 병행, 세대·권한·자원 시험 | 별도 ID의 독립 Observer 유지. 진행 중 handle을 옮기지 않음 |
| NET56-6 | QoS schema/Capability·admission·진단 | 100Hz 요구, 불충족·구 peer 거부, loss/freshness 시험 | 새 기능 OFF, legacy telemetry만 사용. 엄격 명령 경로는 중지 |
| NET56-7 | 배포 및 혼합 버전/LAN 안정성 검증 | 설치·업그레이드·재부팅, 실제 2+장치, 장애 복구 및 기존 회귀 | 이전 바이너리/설정으로 복귀하되 ID·기존 설정 보존, Action 재실행 금지 |

NET56-4 전에 서비스 계정·정책 schema·권한 경계를, NET56-5 전에 local facade 규약을,
NET56-6 전에 QoS wire와 호환 협상을 각각 검토 승인한다. 단계마다 bounded 자원·Stop 및
stale Session 거부를 검증하며 자동 동작이 Local API/SHM 동작을 바꾸지 않는지 확인한다.

보존할 v5.5 계약:

- Local-first IPC, Topic Depth/KEEP_LAST·독립 cursor, Parameter/Service/Action API 및 storage.
- Supervisor ERROR/History, 명시 Reset/Shutdown 및 Managed/External 소유권. Network 서비스 실패만으로
  Application ERROR를 자동 주입하지 않는다. Application은 자신의 통신 의존 실패를 판단할 수 있다.
- 정확한 Endpoint/Session/전체 TypeIdentity, canonical scalar/fixed-array, stale proxy 소유권 검증.
- bounded worker/connection/route/buffer, latest Feedback/Topic, Cancel ACK와 CANCELED 구분.
- 변경 요청 no-retry, Action no-reattach, Topic 명시 재선택, Diagnostics opt-in 및 Counter 의미.

기존 검증 한계는 해소됐다고 가정하지 않는다. RemoteTopicClient Peer 준비 간헐적 Timeout,
물리 LAN Discovery 지연·재시작 복구 안정성, PREEMPT_RT 실장 미검증은 각 후속 시험과 구별한다.
일반 LAN 시험 PASS가 hard real-time 또는 전체 장애 복구 보증을 뜻하지 않는다.

## 9. 구현 전 결정이 필요한 사항

| 항목 | 이 문서의 권장 방향 / 남은 결정 |
| --- | --- |
| ID 파일/계정 | `/var/lib/kcf/host-id` 원자적 보존. 실행 UID/GID, 파일 권한, import/purge·복제 복구 도구 상세 |
| 중복 ID 진단 | 로컬 lock + 원격 충돌 의심 보고, 자동 재발급 금지. 관측 창·차단 범위·해제 절차 |
| NIC | 단일 NIC, 모호하면 명시 설정. route 우선순위·VPN 제외·debounce/backoff 수치 |
| 서비스 | 별도 프로세스/OS 서비스 관리. unit 이름, restart 제한, 정지 timeout 및 backend 정지 시 최종 종료 정책 |
| 정책 schema | 발견/노출/작업 허용 분리·default deny. selector 우선순위, reload/철회 원자성, legacy gateway 정책 migration |
| 로컬 facade | Unix-domain 및 OS credential 검증. API/version·직렬화·client quota·handle 소유권·서비스 내부 Topic 연결 경계 |
| QoS | 명시 요구의 silent downgrade 금지. field 단위·범위, capability/tag, admission lease 및 INTEREST 연계·port 전달 |
| 시간 | 초기 수신 clock 기반 deadline/age. 원본 측정 age용 clock sync는 별도 요구가 있을 때 결정 |
| Tools | v0.14 독립 모드 보존. 신규 adapter 배포 순서·서버 기능 탐지·권한 UI는 별도 저장소 작업 |

## 10. NET56-1 확인 범위

지정 공개 헤더와 네 구현 파일을 정적으로 대조했다. 신규 설계 문서의 상대 링크 및
`git diff --check`를 확인하는 문서-only 단계이다. 빌드·Network/LAN 시험은 재실행하지 않으며
새 PASS 기록, v5.6 구현 완료 또는 버전 배포를 주장하지 않는다.
기존 08/10 문서의 역사적 내용은 수정하지 않는다.
