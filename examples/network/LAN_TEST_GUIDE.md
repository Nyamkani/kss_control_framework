# Physical LAN Action / Topic / Diagnostics

이 문서는 실제 PC ↔ Orange Pi 5+ 검증 절차다. **물리 LAN, Orange Pi 및 실제 GUI 수동 검증은 아직 미실행**이며 아래 출력은 예상 예시다. Loopback 결과를 물리 LAN PASS로 기록하지 않는다.

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

## 검증 기록

- 개발 환경 Debug target build, CLI help 및 잘못된 인자 5개 검사: **PASS**.
- 별도 SHM을 가진 격리 loopback Host 간 실제 전송, callback/counter 증가, 무구독 전송 억제, LOST 관측, SIGINT/SIGTERM 종료, Subscriber 및 양 Host 재시작: **PASS**. 로그: `/tmp/kcf-lan-topic-smoke.log`, `/tmp/lan-topic-*.log`. 물리 LAN 결과와 별개다.
- 실제 물리 LAN / Orange Pi / Tool GUI: **미실행**. 실제 실행 이후 장치·커밋·명령·관측값·오류와 함께 별도 PASS/FAIL 기록을 추가한다.
