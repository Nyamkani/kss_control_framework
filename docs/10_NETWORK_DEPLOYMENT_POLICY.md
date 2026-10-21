# KCF Network Deployment Policy v1.0

대상: KCF Framework v5.4, dev working tree. 정책 작성일: 2026-10-21.

최초 작성 및 아래 구현 대조·현장 보고의 기준은 v5.3이다. 이번 변경은 정책 대상 버전을
v5.4로 정합화하며 당시 검토·검증 결과를 v5.4 재실행 결과로 바꾸지 않는다.

이 문서는 다중 장치 운영과 향후 Debian 패키징의 기준이다. **배포 정책 v1.0은 Network wire/protocol 버전이 아니다.**
현재 구현과 제안 정책을 구분하며, 아래 기본값이나 파일 경로를 이번 문서 작업에서 자동 적용하지 않는다.
구현·과거 검증 이력은 [Network Architecture](08_NETWORK_ARCHITECTURE.md)에 보존한다.

## 1. 다중 장치와 Gateway 운영

- 각 장치에 고유한 Host Identity를 부여하고, Linux Host 하나에는 기본적으로 Gateway 하나를 운영한다.
- 여러 Application/Element는 공통 Gateway를 사용한다. Gateway가 Local introspection을 수집하고,
  설정된 REMOTE endpoint만 외부에 노출한다. Application마다 Gateway를 자동 생성하는 정책이 아니다.
- 서로 다른 IP의 장치는 같은 포트 번호를 사용할 수 있다.
- 같은 Linux Host에서 독립 Gateway를 추가해야 한다면 서로 다른 논리 Host ID와 충돌하지 않는
  bind 주소/포트를 사용한다. **동일 Host ID로 Gateway를 중복 실행하지 않는다.**
- Discovery multicast 수신 포트는 예외적으로 같은 discovery domain에서 공유한다.
  현재 구현은 SO_REUSEADDR로 multicast 수신 socket을 만들지만, 이를 Control/Topic socket의
  충돌 회피 수단으로 일반화하지 않는다.
- 동일 이름의 Topic/Parameter도 Host/Boot/Runtime/Endpoint identity로 구분한다.
  이름만으로 특정 원격 인스턴스를 선택하지 않는다. 기존 Gateway의 이름 기반 Topic 경로는
  후보가 여러 개이면 모호성을 거부하며, Generic RemoteTopicClient의 정확한 Endpoint 선택과 구별한다.

현재 PeerDiscovery 계약은 Host ID당 한 인스턴스다. 중복 실행을 운영 정책으로 금지하는 것이며,
라이브러리가 장치 전체 singleton lock이나 중복 Host ID 자동 재발급을 제공하는 것은 아니다.
동일 PC의 별도 Gateway와 Tool Discovery도 서로 다른 논리 Host ID를 사용하고,
Tool 내부 Diagnostics/Control/Topic Client는 하나의 Discovery를 공유한다.

## 2. 배포 기본 포트

| 기능 | 프로토콜 | 제안 배포 기본값 | 현재 구현과의 관계 |
| --- | --- | --- | --- |
| Discovery | UDP Multicast | 239.255.75.67:37651 | DiscoveryConfig의 현재 기본값과 일치 |
| Gateway Control | TCP | 39001 | metadata_port를 명시해야 함; 현재 기본은 0 |
| Gateway Metadata | UDP | 39001 | Control과 같은 숫자의 별도 UDP socket |
| Gateway Topic | UDP | 37652 | TopicConfig의 현재 기본 포트; Topic 기능은 기본 OFF |
| Tool Control Client | TCP | OS 동적 source port | outbound 연결; Control listener 생성 안 함 |
| Tool Metadata Client | UDP | OS 동적 | 공유 PeerDiscovery의 metadata_port=0 |
| Tool Topic Client | UDP | OS 동적 | RemoteTopicConfig::receive_port=0 |

위 값은 **배포 기본 정책**이며 Host별 설정으로 변경할 수 있어야 한다.
현재 Framework가 39001이나 향후 설정 파일을 자동 적용한다고 해석하지 않는다.
PC Gateway도 별도 IP라면 39001을 사용할 수 있으며 39002는 필수값이 아니다.

ControlNode는 TCP listener를 먼저 bind하고, 실제 선택된 포트를 Discovery의 UDP metadata
socket에 지정한다. 따라서 metadata_port=0이면 OS가 TCP 포트를 동적 할당하고, UDP는 그
숫자에 별도로 bind한다. UDP bind 충돌 시 실패할 수 있으며 두 프로토콜의 포트를 원자적으로
예약하는 것은 아니다. PeerDiscovery 단독 실행에서는 metadata_port=0으로 UDP를 직접 동적 할당한다.

Local ServiceServer의 내부 IPC 포트는 Gateway 외부 Control 포트와 별개다.
Service/Action마다 LAN 수신 포트를 새로 공개할 필요는 없으며, 예제의 내부 ServiceServer
포트를 외부 Gateway Control 포트로 설정하지 않는다.

### 설정 변경의 현재 제약

- Discovery 참여자는 같은 multicast group/port와 접근 가능한 인터페이스를 사용해야 한다.
- 기존 이름 기반 Gateway Topic 경로의 TopicConfig::port는 수신 bind와 원격 송신 목적지에
  함께 쓰인다. 참여 Gateway는 공통 Topic 포트를 전제로 한다. 같은 IP의 독립 Gateway마다
  포트만 다르게 설정해도 기존 경로의 상호 통신이 자동 지원되는 것은 아니다.
- Generic RemoteTopicClient는 publisher_port와 receive_port를 구분한다. 검증된 INTEREST의
  UDP source port로 DATA를 반환하므로 별도 동적 수신 포트로 Gateway와 공존할 수 있다.
  publisher_port는 Client 인스턴스 단위 설정이며 Publisher별 임의 포트를 자동 협상하지 않는다.
- Host별 포트 변경은 상대측 설정과 함께 검증해야 한다. 동일 PC의 복수 Gateway가 필요하면
  별도 IP/네트워크 격리 또는 Topic 경로 구성 검토가 선행되어야 한다.

## 3. Host Identity와 생명주기

**설계 정책:** Host ID는 장치 최초 설정 시 생성하고 영구 보존한다. hostname이나 DHCP 주소로
매번 다시 만들지 않는다. 이미지 복제 시 원본 Host ID를 그대로 배포하지 않도록,
이미지 제작 단계에서 제외하거나 새 장치의 최초 provisioning에서 발급하는 절차가 필요하다.

**현재 구현:** HostIdentity는 0이 아닌 128-bit 값이며 호출자가 제공한다.
라이브러리는 자동 Identity Provisioning이나 영구 저장소를 제공하지 않는다.
예제는 명시적 Host ID와 Linux boot_id를 사용한다.

- Boot ID는 부팅 세대를 구분한다. 기존 Framework의 boot identity 계약을 유지한다.
- PeerDiscovery는 Start마다 새 random 64-bit Session ID를 만든다.
- RemoteRuntimeIdentity는 Host + Boot + PID + process_start_ticks,
  RemoteEndpointIdentity는 여기에 registration_id를 포함한다.
- DHCP IP 변경 자체는 Host ID 변경을 의미하지 않는다. 다만 현재 Discovery는 주소/포트나
  Boot/Session 변경을 기존 peer timeout 이후 수용하므로 즉각적인 무중단 이동은 보장하지 않는다.
- Gateway 재시작으로 Session이 바뀌면 기존 선택의 expected_session은 유효하지 않다.
  Application Runtime이 그대로여도 기존 요청/Action Handle을 새 Gateway 세대에 자동 연결하지 않는다.
  기존 Endpoint identity가 반드시 재발급된다는 의미가 아니라, **Endpoint와 선택한 Session의 조합을 재검증**해야 한다.
- 새 발견/새 요청 또는 명시적 Topic 재선택과, 기존 Action의 자동 재실행·reattach는 구별한다.
  Timeout/Disconnect 이후 실행 결과가 불명확한 변경 요청을 자동 재시도하지 않는다.

## 4. 방화벽·인터페이스·보안

아래는 제안 기본 포트를 사용한 경우의 트래픽 구분이다. 실제 설치 설정에 맞춰 검토한다.

| 장치 역할 | 검토할 수신 트래픽 |
| --- | --- |
| Gateway | Discovery UDP 37651, Metadata UDP 39001, Control TCP 39001, Topic 사용 시 UDP 37652 |
| Tool 전용 | Discovery UDP 37651, 공유 Discovery의 동적 Metadata UDP 응답, 동적 Topic UDP DATA, outbound TCP 연결의 응답 |

Tool 전용 장치에는 Gateway TCP listener가 필요하지 않다. 그러나 모든 수신 UDP가 불필요한
것은 아니다. Tool Metadata 요청/응답과 Topic INTEREST/DATA의 실제 주소·포트를 확인한다.
UFW 또는 하위 방화벽의 연결 추적이 동적 UDP 응답을 항상 허용한다고 가정하지 않는다.
특히 multicast announce 이후 다른 주소/포트로 오는 metadata와 UDP 상태 유지 시간은
실제 규칙·패킷 캡처로 검증한다. 고정 수신 포트가 필요한 환경은 충돌 및 상대 포트 계약도 확인한다.

- 허용 범위는 신뢰된 LAN/Subnet과 선택한 NIC로 제한한다. 인터넷에 직접 노출하지 않는다.
- multicast TTL은 현재 1이다. VLAN/라우터 경계를 자동 통과하지 않으며, 다른 VLAN 간 운영은
  네트워크 구성과 지원 범위의 별도 검토 대상이다.
- Wi-Fi client isolation, multicast filtering, VM bridge/NAT, 스위치 multicast 설정으로
  Discovery가 차단될 수 있다. unicast 연결 성공만으로 Discovery 수신을 보장하지 않는다.
- 다중 NIC에서는 사용할 IPv4 interface_address를 명시하고 multicast 송수신 경로를 확인한다.
  현재 자동 NIC 선택·다중 NIC failover를 배포 기능으로 가정하지 않는다.
- Host ID와 Allowlist는 암호학적 인증이 아니다. 기존 LAN 프로토콜은 인증·암호화를 제공하지 않는다.
- Endpoint REMOTE 노출과 Descriptor/Get/Set/Service/Action 권한은 별개다.
  Tool 권한은 필요한 개별 Allowlist에만 명시하고, Diagnostics도 명시적 opt-in을 유지한다.
- 설치 시 방화벽을 무조건 자동 개방하지 않는다. 선택적 도구도 적용 전 인터페이스·범위·포트를
  보여주고 운영자가 선택하도록 설계한다.

## 5. 향후 Debian 패키징 — 미구현 설계안

다음은 구현 완료 항목이 아니다. 현재 Network 공개 API와 예제 실행 파일을
설정 가능한 상시 운영 Gateway 패키지가 이미 존재하는 것으로 해석하지 않는다.
관련 헤더, Network 예제 target 및 패키징 파일명 확인 범위에서는 아래 배포 구성의 구현을 확인하지 못했다.

| 제안 구성 | 설계 방향 / 결정 필요 사항 |
| --- | --- |
| /etc/kcf/network.conf | NIC, multicast, Control/Metadata/Topic 포트 및 opt-in 설정. 문법·검증·설정 우선순위·reload 범위 결정 |
| /var/lib/kcf/host-id | 최초 생성 후 영구 보존. 형식·권한·원자적 생성·복제 이미지 초기화·삭제/purge 정책 결정 |
| systemd Gateway 서비스 | 운영용 실행 파일과 설정 로더부터 구현 필요. 실행 계정·Local registry/SHM 접근권한·의존성·종료 시간 결정 |
| 최초 설치 provisioning | 중복 ID 방지, 오프라인 설치, 재설치 시 기존 ID 재사용, 실패 시 복구 절차 결정 |
| 업그레이드 | 기존 설정/ID 보존, 설정 마이그레이션, 서비스 재시작 시점과 Action 중단 영향 명시 |
| 중복 실행 방지 | 서비스 단일 인스턴스 관리 및 수동 실행과의 lock 정책. bind 실패만으로 중복 ID를 검출했다고 간주하지 않음 |
| 선택적 방화벽 도구 | 관리자 명시 선택, subnet/NIC 제한, 동적 UDP 정책, 적용 취소 및 기존 규칙 보존 |
| 설치 후 확인 | 설정 유효성, ID/포트 충돌, multicast 수신, metadata, 최소 권한 호출, Topic 경로와 종료 상태 확인 |

현재 /etc/kcf/network.conf 로더, 영구 Host ID provisioning, 배포용 Gateway systemd 서비스,
Debian maintainer script가 자동 제공된다고 문서화하지 않는다. 이번 작업은 이를 구현하지 않는다.

## 6. 물리 LAN에서 확인된 교훈

다음은 **사용자가 이번 요청에서 보고한 현장 결과**이다. 이번 문서 작성 중 새로 수행한 시험이나
전체 안정성 인증을 뜻하지 않는다. 장치별 로그·시험 조건의 추가 수집은 별도 작업이다.

- VM 방화벽에서 Discovery UDP 37651이 차단되어 Peer LOST가 발생했다.
- 동일 Host ID로 Gateway 두 개를 실행했을 때 Endpoint 발견 문제가 발생했다.
- Topic/Parameter/Service/Action의 1차 물리 LAN 기능 검증을 완료했다.
- 재시작 후 재발견 및 자동 복구 안정성은 추가 검증이 필요하다.

여기서 자동 복구 검증은 발견·연결·Route의 각 정책을 구분해야 한다.
RemoteTopicClient의 명시적 재선택, Action의 재연결 금지, 변경 요청의 자동 재시도 금지는 유지한다.
이 기록으로 과거 문서의 “물리 LAN 미검증” 당시 기록을 소급 변경하지 않는다.

## 7. 구현 대조 및 문서 관계

대조한 공개 계약:
[PeerDiscovery](../kcf/include/kcf/network/peer_discovery.hpp),
[ControlNode](../kcf/include/kcf/network/control_node.hpp),
[TopicDataPlane](../kcf/include/kcf/network/topic_data_plane.hpp),
[RemoteTopicClient](../kcf/include/kcf/network/remote_topic_client.hpp).
포트 구현은 해당 Network 구현의 bind/getsockname/multicast 설정 및 Client I/O bind 부분을 확인했다.

| 구분 | 검토 결과 |
| --- | --- |
| 현재 구현과 일치 | Host ID당 Discovery 한 개, TCP/UDP 동일 숫자 Control/Metadata, port=0 동적 할당, multicast TTL=1, Topic opt-in, 독립 Client 권한 |
| 배포 정책이며 미구현 | 장치당 공통 Gateway의 자동 운영, 영구 ID 발급·보존, 39001 기본 적용, 설정 로더, systemd/Debian 설치·업그레이드 자동화 |
| Architecture와 중복 | Identity, Discovery lifecycle, Control 포트 결합, Topic 포트 및 권한 계약. 이 문서는 설치·운영 기준만 보완 |
| 역사적 표현 주의 | Architecture의 초기 ephemeral metadata 설명은 이후 metadata_port 설정 설명과 함께 읽는다. 기존 Gateway의 공통 Topic 포트 설명과 새 Client의 동적 수신 포트는 서로 다른 경로다 |
| 검증 상태의 시점 차이 | README/Architecture의 과거 물리 LAN 미검증 서술과 이번 현장 보고를 구분한다. 이번 수정 범위에서는 기존 본문·검증 이력을 변경하지 않는다 |

Debian 패키징 전에는 운영용 Gateway 실행 파일/설정 schema, persistent ID 발급과 복제 정책,
단일 인스턴스 보장, 동적 UDP 방화벽 정책, 같은 PC의 복수 Gateway Topic 구성,
서비스 계정의 Local IPC 접근권한, 업그레이드·재시작 시 진행 중 Action 처리 및 복구 시험 기준을 확정해야 한다.

문서-only 작업이며 새 build/test PASS를 추가하지 않는다. 기존 Network wire/storage/API 버전은 변경하지 않는다.
