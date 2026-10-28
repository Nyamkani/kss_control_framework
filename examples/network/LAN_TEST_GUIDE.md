# Physical LAN Action / Topic / Diagnostics

이 문서는 실제 PC ↔ Orange Pi 5+ 검증 절차와 단계별 기록이다.
**Historical — 최초 작성 당시 물리 LAN, Orange Pi 및 실제 GUI 수동 검증은 미실행**이었다.
아래 절차의 출력은 예상 예시이며 Loopback 결과를 물리 LAN PASS로 기록하지 않는다.
후속 사용자 수동 결과는 [2026-10-21 물리 LAN 검증](#physical-lan-2026-10-21)에 별도로 기록한다.

## 준비 및 빌드

두 Linux 장치에서 같은 Framework 소스로 빌드한다. 아래 IP는 예시이며 실제 LAN interface 주소로 바꾼다.

- PC: `192.168.1.10`, Host ID `22222222222222222222222222222222`
- Orange Pi: `192.168.1.20`, Host ID `11111111111111111111111111111111`
- Host ID는 서로 달라야 하며 동시에 같은 ID의 Gateway를 두 개 실행하지 않는다.
- Action과 Topic 절차는 각각 종료한 뒤 실행한다. 같은 포트를 사용하는 두 CLI를 동시에 실행하지 않는다.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --target kcf_lan_action kcf_lan_topic -j4
```

## Remote Action Goal / Cancel

Orange Pi:

```bash
./build/examples/network/kcf_lan_action owner \
  11111111111111111111111111111111 192.168.1.20 37651 39001
```

PC:

```bash
./build/examples/network/kcf_lan_action client \
  22222222222222222222222222222222 192.168.1.10 37651 39002 \
  11111111111111111111111111111111 goal 7

./build/examples/network/kcf_lan_action client \
  22222222222222222222222222222222 192.168.1.10 37651 39002 \
  11111111111111111111111111111111 cancel 7
```

예상: `Goal accepted` → `Feedback sequence=...` → `SUCCEEDED result=14 PASS`.
Cancel은 Feedback 관측 후 요청하며 `Cancel ack error=0`과 최종 `CANCELED result=-1 PASS`를 별도로 확인한다.
Timeout/disconnect는 원격 실행 취소를 보장하지 않는다. 자동 재시도하지 않는다.

## Remote Topic

Orange Pi Publisher:

```bash
./build/examples/network/kcf_lan_topic publisher \
  11111111111111111111111111111111 192.168.1.20 37651 39001 37652
```

PC Subscriber:

```bash
./build/examples/network/kcf_lan_topic subscriber \
  22222222222222222222222222222222 192.168.1.10 37651 39002 37652
```

두 장치는 **Discovery UDP port와 Topic UDP port가 각각 같아야 한다**. 각 CLI의 세 포트는 서로 달라야 한다. Local IPv4는 해당 장치에 할당된 unicast 주소다.

`/network/test_topic`에 기존 `parameter_demo_type.hpp`의 `Value`와 descriptor를 그대로 사용한다.
`count`는 이 예제의 application sequence이며 성공한 Local Publish마다 증가한다. `enabled=true`, `gain=count*2`다. Transport sequence와 혼동하지 않는다.
Publisher는 ProcessRuntime의 약 200 Hz Loop에서 Publish하며 Gateway `max_hz=50`이다. hard real-time 보장은 없으며 실제 전송은 50 Hz보다 낮을 수 있다. 카운터는 누적값이며 1초 간격 출력 차이로 대략적인 주기를 확인한다.

예상 출력(수치는 실행마다 다름):

```text
Ready mode=publisher topic=/network/test_topic diagnostics=ON
Status published=1000 publish_busy=0 received=0 latest=0 ... send_routes=1 ...
Route ACTIVE sending=1 id=1 source_session=... destination_session=... sent=150 ... skipped=450 ...

Proxy ready; Subscriber callback attached
Status published=0 publish_busy=0 received=150 latest=995 duplicate=0 backwards=0 invalid=0 ... receive_routes=1
Route ACTIVE sending=0 id=1 source_session=... destination_session=... received=150 proxy_published=150 ...
```

Subscriber는 Gateway의 Proxy 준비를 확인한 뒤 실제 callback을 등록한다. callback의 중복·역행·값 오류는 fatal 검증 실패다. 순번의 **건너뜀은 정상**이다: Gateway sampling과 latest-only callback은 모든 200 Hz 메시지를 전달하지 않는다. Proxy Publish와 callback 횟수도 같은 의미가 아니다.
Subscriber가 없을 때 Publisher의 Local count는 계속 증가하지만 Topic payload 전송은 발생하지 않아야 한다. Discovery/Control traffic은 계속 존재한다. Subscriber는 상대를 기다리는 장기 실행 CLI이며 상대 부재 자체를 실패 처리하지 않는다. Ctrl+C로 종료할 수 있다.

## Diagnostics / KCF Tools v0.13

두 Topic Gateway 모두 `diagnostics_enabled=true`, 정확한 Topic allowlist와 descriptor를 사용한다. CLI는 공개 `GetTopicDiagnostics()`의 실제 Route 상태와 9개 Counter를 출력한다. 외부 Tool은 별도 read-only Diagnostics API를 사용한다.

현재 Framework의 Diagnostics API를 포함해 빌드한 KCF Tools v0.13에서 실행한다(경로는 Tool 저장소 기준):

```bash
./build-network/kcf_tool --backend kcf \
  --network-interface 192.168.1.10 --network-port 37651
```

수동 Refresh 후 두 Host와 Remote Topic Detail을 확인한다.

- 양 Gateway Diagnostics가 유효하며 Send/Receive/Proxy Publish Hz와 Samples Skipped/s가 표시되는지 확인한다. 첫 표본은 rate 측정 대기일 수 있다.
- 양쪽 ACTIVE Route의 route ID, source/destination session과 전체 endpoint identity가 일치하는지 확인한다. 숫자 route ID만 같은 것으로 판정하지 않는다.
- Graph에서 Publisher → Subscriber의 실제 Network Route Edge와 tooltip을 확인한다.
- Local Publish 약 200 Hz와 Network Send 최대 50 Hz는 서로 다른 값이다. Proxy Publish Hz는 Application Subscriber 처리율이 아니다.
- Tool GUI는 수동 Refresh이며 Graph의 10초 freshness 판정도 Refresh/재구성 시 적용된다. 신규 Timer/자동 UI 갱신 기능을 의미하지 않는다.

자세한 계약: [Network Architecture](../../docs/08_NETWORK_ARCHITECTURE.md).

## 방화벽 / 네트워크

신뢰할 수 있는 같은 LAN에서 다음 양방향 통신을 허용한다.

| 용도 | 프로토콜 / 예시 포트 |
| --- | --- |
| Discovery multicast | UDP `239.255.75.67:37651` |
| Metadata / Control | 각 Gateway의 UDP **및** TCP `39001` 또는 `39002` |
| Topic payload / interest | UDP `37652` (양쪽 같은 설정) |
| Tool Discovery metadata | Tool의 동적 UDP metadata port 및 응답 traffic |

Tool에서 Gateway TCP로 접속할 수 있어야 한다. Linux 방화벽, Wi-Fi client isolation, multicast 차단, VLAN 분리 여부를 확인한다. NAT/WAN이나 인증·암호화 검증용 예제가 아니다.

## LOST / Restart 절차

1. Publisher만 실행해 Local count 증가와 Topic payload send=0을 확인한다.
2. Subscriber 시작 후 callback과 양쪽 ACTIVE counter 증가를 확인한다.
3. Subscriber를 Ctrl+C/SIGTERM으로 종료한다. Publisher/Tool에서 peer timeout(기본 약 4초) 이후 LOST와 ACTIVE Route 해제를 관측한다.
4. Subscriber를 같은 명령으로 재시작한다. 새로운 session/endpoint로 Route가 만들어지고 callback이 다시 증가하는지 확인한다.
5. Publisher를 종료해 Subscriber/Tool에서 LOST를 관측한다. 그 뒤 Subscriber도 종료하고 Publisher → Subscriber 순서로 재시작한다. application count는 새 Publisher에서 다시 1부터 시작한다.
6. Publisher만 재시작하면 기존 Subscriber는 source identity 변경을 `-ESTALE`로 보고 종료할 수 있다. 자동 재연결을 가정하지 말고 Subscriber를 명시적으로 재시작한다. 기존 Local mapping을 임의로 삭제하지 않는다.
7. 양쪽 `Shutdown complete`를 확인한다. Tool은 Refresh 후 새로운 session과 Graph를 확인한다.

Runtime이 signal 처리와 Shutdown을 수행한다. Subscriber callback worker를 먼저 join하고 Gateway를 Stop한 뒤 직접 소유한 Publisher를 Unlink/Close한다. Gateway Proxy 저장소 수명과 stale 복구는 기존 Framework 계약을 따른다.

## Historical — 최초 CLI 작성 당시 검증 기록

- 개발 환경 Debug target build, CLI help 및 잘못된 인자 5개 검사: **PASS**.
- 별도 SHM을 가진 격리 loopback Host 간 실제 전송, callback/counter 증가, 무구독 전송 억제, LOST 관측, SIGINT/SIGTERM 종료, Subscriber 및 양 Host 재시작: **PASS**. 로그: `/tmp/kcf-lan-topic-smoke.log`, `/tmp/lan-topic-*.log`. 물리 LAN 결과와 별개다.
- 당시 실제 물리 LAN / Orange Pi / Tool GUI: **미실행**. 후속 수동 검증은 아래 별도 기록이며 당시 결과를 소급 변경하지 않는다.

<a id="physical-lan-2026-10-21"></a>

## 2026-10-21 물리 LAN 수동 검증 — 사용자 확인

다음은 **사용자가 보고한 실제 수동 검증 결과**다. 이번 문서 작업에서 새로 실행한 시험이나
기존 자동 테스트 결과를 물리 LAN 결과로 바꾼 기록이 아니다.

- 환경: Orange Pi 5+ ↔ Ubuntu VM, Bridged LAN, 실제 KCF Tools GUI.
- Framework v5.3: dev commit `9e9cc1b`.
- KCF Tools v0.13: main commit `e332438`.
- 위 SHA는 검증 기준이며 향후 branch HEAD를 계속 추적하는 표기가 아니다.
- Action Cancel 시험에는 아래의 Orange Pi 전용 10초 설정 변경이 포함된다.
  따라서 해당 Cancel 결과를 공개 commit 무수정 상태에서 실행한 결과로 해석하지 않는다.

| 검증 항목 | 사용자 확인 결과 |
| --- | --- |
| Discovery / Metadata | PASS |
| Remote Topic Subscribe / 실제 Payload 수신 | PASS, 약 45.96 Hz 관측 |
| Parameter Get / Set / Readback | PASS |
| Service Call / Response | PASS |
| Action Goal / Feedback / SUCCEEDED | PASS |
| Action Cancel ACK / CANCELED Terminal | PASS |

최종 Cancel Result는 `count=-1, enabled=false, gain=1.5`이며,
`execution_unknown=0`, `cancel_unknown=0`을 확인했다.
Cancel ACK 수신과 CANCELED Terminal 확인은 별도 단계다.
약 45.96 Hz는 해당 시험 환경의 Tool 관측 수신률이며 보장 전송률이나 Application 처리율이 아니다.

### 문제와 시험 설정 이력

1. Ubuntu VM의 UFW가 Discovery UDP 37651을 차단해 Peer LOST가 발생했다.
2. 동일 Host ID로 Gateway 두 개를 실행해 Endpoint 발견 문제가 발생했다.
3. Action Owner 예제의 2초 완료 시간 때문에 GUI에서 수동 Cancel을 보내는 시점과 완료가 경합했다.
4. Orange Pi에서 시험용 완료 시간을 10초로 변경한 뒤 Cancel 시험에 성공했다.
   **GitHub Action 예제 소스의 설정은 여전히 2초**다. 이번 문서 작업은 예제 코드를 변경하지 않으며,
   10초 시험 설정을 기본 동작이나 Tool timeout 변경으로 설명하지 않는다.
5. 일시적 통신 장애 이후 양쪽 프로그램을 재시작해야 했던 사례가 있었다.
   문제를 정리하고 양쪽을 전체 재시작한 뒤 정상 연결과 최종 Cancel 결과를 확인했다.
   이 결과는 한쪽만 재시작했을 때의 자동 복구 성공을 뜻하지 않는다.

위 기존 CLI 명령은 절차 예시이며 사용자가 실행한 전체 명령 로그를 재구성한 것이 아니다.
Tool 제어를 사용할 때에는 실제 Tool Host ID에 Descriptor와 작업별 권한을 부여해야 한다.
Parameter Owner의 Descriptor/Get/Set, Service Owner의 Descriptor/Call,
Action Owner의 Descriptor/Goal/Cancel 권한은 서로 독립적이다.
Tool에서 정확한 Remote Host/Endpoint를 선택하고, 동일 ID의 Gateway를 중복 실행하지 않는다.

### 추가 검증이 필요한 항목

- Gateway 단독 재시작 후 재발견·복구.
- Tools 단독 재시작 후 재발견·복구.
- 일시적 LAN 단절/장애 이후 자동 복구.
- 실제 다중 Gateway 동시 제어와 장시간 운영 안정성.

Multi-Host 선택 UI 지원은 위 다중 Gateway 물리 시험의 완료를 뜻하지 않는다.
기존 Action의 자동 reattach 및 변경 명령 자동 retry는 지원하지 않는다.
앞 절의 LOST/Restart 절차와 Virtual LAN PASS를 위 미검증 항목의 실행 결과로 대신하지 않는다.
**1차 기능 PASS를 전체 네트워크 안정성 검증 완료로 해석하지 않는다.**

포트와 Host Identity 운영은 [Network Deployment Policy v1.0](../../docs/10_NETWORK_DEPLOYMENT_POLICY.md)을 따른다.
예시 PC의 39002는 필수값이 아니며 별도 IP라면 Gateway Control/Metadata에 39001을 사용할 수 있다.
Gateway의 공통 Topic UDP 포트와 Tool RemoteTopicClient의 동적 수신 포트는 구분한다.
Tool의 동적 UDP 응답이 방화벽에서 무조건 허용된다고 가정하지 않는다.
영구 Host ID provisioning 및 Debian/systemd 자동 설정은 향후 설계이며 현재 자동 적용되는 기능이 아니다.

<a id="net56-lan-20261028"></a>
## v5.6.0 정합화 — OPI5+ 물리 LAN 사용자 보고 (2026-10-28)

출처는 **사용자 수동 확인**이며 이번 작업에서 물리 LAN을 재시험한 결과가 아니다.
Framework 버전을 5.5.0에서 **5.6.0**으로 정합화한다. 시험 당시 개발 표기와 이번 버전 적용을
구분하며 정식 Release·Tag·배포 완료를 선언하지 않는다. NetworkProtocol 1.0, Discovery v2,
Control TCP v1, Topic wire v1 및 기존 IPC 계약은 유지한다. EXACT_FRAMEWORK_VERSION 정책에서
5.5.0과 5.6.0은 불일치하며 정책 자체는 변경하지 않는다.

- OPI5+: Ethernet `192.168.1.155`, Wi-Fi `192.168.1.147`.
- Ubuntu VM: `192.168.1.193`, KCF Tools v0.14.0.
- Local Publisher 약 200Hz, Publish Busy 0 관측.
  이 Publisher는 **OPI5+ 로컬 시험 변경**이며 저장소의 기본 1Hz 예제가 200Hz를
  지원·검증했다고 해석하지 않는다.

| Gateway 설정 | 유선 Tools Receive | Wi-Fi Tools Receive | 판정 |
| --- | --- | --- | --- |
| 50Hz | 목표 주기 근접 (정확한 수치 미제공) | 48~52Hz | 수신률 1차 PASS |
| 100Hz | 목표 주기 근접 (정확한 수치 미제공) | 99~102Hz | 수신률 1차 PASS |
| 200Hz | 목표 주기 근접 (정확한 수치 미제공) | 199~205Hz | 수신률 1차 PASS |

Local Publish와 Tools Receive는 별도 지표다. UDP 패킷 손실률, p95/p99 Latency,
장시간 Topic 안정성은 미검증이다. 샘플 수·최대 수신 공백·정확한 측정 구간은 이 보고에
제공되지 않았으며 평균 수신률로 hard real-time 도착 보장을 주장하지 않는다.

### Discovery 및 연결 장애 관측

- Wi-Fi Discovery에서 ONLINE/LOST 반복과 멀티캐스트 수신 공백이 재현됐다.
- Tools의 `Apply Network Settings`는 NetworkObserver를 재시작하며 IGMPv3 Report 후 수신 복구를 확인했다.
- 별도 수동 IGMPv3 Membership Report만 송신해도 Gateway/Tools 재시작 없이 Discovery가 즉시 복구됐다.
  이를 제품의 자동 복구 기능으로 해석하지 않는다.
- VM에서 5분간 IGMP Query/Report가 관측되지 않은 시험이 있었다.
  IGMP 그룹 관리·멀티캐스트 전달 경로가 의심되지만 **정확한 장치 원인은 미확정**이다.
- Wi-Fi SSH/Ping 단절 사례가 있으며 유선 연결 시 동일 서브넷의 비대칭 라우팅을 확인했다.
- 유선을 유지한 상태에서 Wi-Fi 인터페이스를 지정한 5분 Ping은 정상이었다.
  이는 **Wi-Fi 단독 장시간 안정성 PASS가 아니다**.
- Graph는 자동 갱신되지 않으며 이전 Snapshot이 남을 수 있다.

### 남은 검증과 릴리스 경계

정상 연결 기능 및 Rate 1차 PASS와 장애 복구 안정성 미검증을 구분한다.
2026-10-27의 P0 Discovery 지연은 위 관측으로 원인을 확정하거나 해결 처리하지 않는다.
P1 미허용 Host의 `ECONNRESET(-104)` / 명시적 EACCES 응답 미검증,
P2 Endpoint 소멸 후 STALE 핸들 해제 결과 불명확 기록도 유지한다.
과거 Service 선택 -ENOENT 및 Real Smoke 6.4/24.9Hz 원인 미확정 기록은 변경하지 않는다.

실제 systemd 부팅·자동 재시작은 **미수행**이며 Gateway 서비스는 **비활성화** 상태를 유지했다.
독립 Orange Pi 5+의 systemd 운영 검증, 실제 LAN Freshness·장애 복구·장시간 측정이 남았다.
Raspberry Pi 5는 매카넘 로봇 전용으로 유지한다. 실제 RT 성능·로봇 Fail-safe도 미검증이다.
버전 정합화와 정식 v5.6.0 Release 완료는 별개다.

운영 경로 준비는 [서비스 비활성 상태의 수동 실행](../../gateway/systemd/README.md#manual-gateway-with-the-service-disabled)을 참고한다.
이 기록은 이전 미실행 이력을 소급 변경하지 않는다.
