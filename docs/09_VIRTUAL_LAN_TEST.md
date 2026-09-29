# 09. Virtual 2-Host LAN integration

이 fixture는 물리적 두 장치 시험을 대신하는 **격리된 Linux Virtual LAN 시험**입니다. 배포용 container나 새로운 Framework 기능이 아닙니다. 기존 localhost Network regression과 별도 명령으로 실행하며 Framework protocol/API, Application, kcf_tools를 수정하지 않습니다.

공개 상태 보충: 이 fixture와 proxy recovery 구현은 이후 v5.2 dev commit
`0a3f2a630cec209793f9ac6e99107752e9e01283`에 최초 반영되었습니다.
후속 version/document alignment는 KCF v5.2의 5.2.0 version-alignment commit
`d41fc2fc71d3b60b0475035ec56ac32b92ffb849`에 반영되었습니다(Framework v5.2 / 5.2.0).
아래 최초/후속 검증 결과와 당시 commit/push 미수행 기록은 historical fact로 유지합니다.
당시 5.2.0 버전 정합성 작업에서는 Virtual LAN 전체 시험을 재실행하지 않았습니다.
이번 공개 상태 문서 마감에서도 재실행하거나 새 PASS를 추가하지 않습니다.

## 구조와 격리

```text
Rootless outer user + PID + network + mount namespace
                     kcfbr0 (IP/uplink/NAT 없음)
                       /                 \
                    veth1              veth2
                      |                  |
Host A: lan0 10.203.0.2/24       Host B: lan0 10.203.0.3/24
  own net / IPC / mount ns        own net / IPC / mount ns
  own tmpfs /dev/shm (64 MiB)     own tmpfs /dev/shm (64 MiB)
  HostIdentity 01…               HostIdentity 02…
  Local owner + Gateway          Local owner + Gateway + Subscriber
```

Ubuntu의 unprivileged user namespace를 사용하므로 허용된 환경에서는 sudo가 필요 없습니다. 현재 실행 환경에서 Docker daemon/image 의존성 없이 `unshare`, `nsenter`, `iproute2`와 `tc netem`으로 재현할 수 있어 이 방식을 선택했습니다. Namespace 내부 root는 user mapping을 사용하며 host 네트워크·방화벽·routing·실제 `/dev/shm`을 수정하지 않습니다. Bridge에는 외부 uplink, default route, NAT가 없습니다. 코드와 build artifact는 기존 파일 시스템에서 실행합니다. 신뢰할 수 없는 프로그램을 실행하는 security sandbox를 제공하는 목적은 아닙니다.

각 Host는 서로 다른 network/IPC/mount namespace와 별도로 mount한 tmpfs를 사용합니다. IPC namespace만 생성해서는 POSIX SHM이 격리되지 않으므로 `/dev/shm` mount 격리가 필수입니다. 테스트는 namespace inode, 양쪽 SHM object 목록의 분리, 상대 Topic/Parameter Local Open의 `-ENOENT`를 확인합니다. Service/Action introspection registry와 Action status SHM도 각 tmpfs에만 존재합니다. 기존 Local Service는 namespace 안의 loopback으로 실행하며 양쪽 owner가 같은 Local Service port를 사용합니다.

Discovery는 `239.255.75.67:37651` multicast를 **lan0 주소**로 송수신합니다. Topic은 UDP 37652, Control은 Discovery로 교환한 TCP port를 사용합니다. Endpoint metadata는 기존 introspection에서 자동 수집합니다. 기존 loopback 테스트의 SHM 이름 interception, payload mock, localhost peer shortcut은 사용하지 않습니다. Controller pipe에는 시험 명령과 관측 결과만 흐릅니다. A의 owner payload를 B의 Subscriber로 보내는 별도 pipe 경로는 없습니다.

Local owner, Gateway, Subscriber는 별도 process입니다. Gateway를 SIGKILL해도 Local owner를 유지하므로 Network 장애가 Local IPC를 멈추지 않는지 실제로 확인합니다.

## 실행

```sh
./tests/network_virtual/run_virtual_network_test.sh
# 기존 build 경로와 결과 디렉터리를 지정할 수도 있습니다.
./tests/network_virtual/run_virtual_network_test.sh build build/network-virtual/manual
```

Runner가 필요한 C++17 fixture target만 configure/build한 뒤 namespace 생성, fixture 실행, 장애 주입, 정리까지 수행합니다. 일반 build는 root나 namespace 권한을 요구하지 않습니다. Virtual LAN 실행은 CTest/default regression에 자동 등록하지 않았습니다.

필요한 도구: CMake/C++17 compiler, Python 3, util-linux의 unshare/nsenter/mount, iproute2의 ip/tc/ss. 먼저 user/network/IPC/mount namespace 권한을 검사합니다. 정책에 의해 막혀 있거나 도구가 없으면 명확한 LIMITATION과 exit 77을 반환합니다. 자동 sudo/password prompt 또는 system 정책 변경은 하지 않습니다. Netem이 없으면 관련 시험만 LIMITATION으로 기록합니다. Functional 실패는 exit 1이며, 물리 LAN 미검증만으로는 실패 exit를 반환하지 않습니다. 증명된 proxy의 SIGKILL recovery 실패는 functional FAIL입니다.

결과 디렉터리에는 `results.json`, `events.jsonl`, `topology.json`, process별 stderr, build/preflight 로그와 netem 통계를 남깁니다. 시험 중 오류가 발생하면 `failure.txt`에 traceback을 기록합니다. 같은 결과 디렉터리를 재사용하면 로그가 덮어써지므로 기본값인 시각/PID별 경로 또는 새로운 경로를 권장합니다.

실패 시에도 fixture process를 종료·wait하고 keeper를 종료합니다. 마지막 namespace reference가 사라지면 해당 tmpfs, veth, qdisc가 제거됩니다. Outer PID namespace의 init 종료와 `unshare --kill-child`도 잔여 child를 정리합니다. 전체 `/dev/shm` 삭제나 host resource에 대한 광범위한 cleanup은 없습니다.

## 시나리오와 판정

- 양방향 multicast Discovery: ONLINE, self 제외, 실제 NIC source 주소, HostIdentity/session, compatible profile와 endpoint metadata 확인. Fixture는 Discovery v2를 compile-time 확인합니다.
- Parameter: Get/Set와 Local storage 실제 변경, LOCAL-only/type mismatch 거부, 역방향 Get.
- Service: 실제 Local handler 실행 횟수 및 request/response 값 확인. 장치 제어는 없습니다.
- Action: accepted, 여러 feedback 관측, terminal result, 별도 Cancel ack와 terminal CANCELED. ACCEPTED 단계의 Cancel도 처리합니다.
- Topic: Subscriber 전 무전송, proxy 준비 후 기존 Local Subscriber 수신, payload/type/monotonic sample 및 Local transport sequence 검사. Local 약 200 Hz와 기본 Remote 최대 50 Hz를 분리해서 측정합니다. Publisher를 잠시 멈추면 같은 Local sequence를 다시 전송하지 않아야 합니다.
- 동시 부하: Topic을 계속 발행하면서 Parameter Get/Set, Service, Action Goal/Cancel 완료 확인.
- SIGKILL/restart: peer LOST, route 제거, Remote request 실패, 원래 Local owner의 read/publish 지속. 새 session과 새 route로 재연결 후 모든 Remote operation 재검증.
- 실제 `lan0 down/up`: LOST와 route 제거, namespace 안 `ss`로 TCP established 연결 제거, interface 복구 후 rediscovery 및 operation 복구 확인. `ControlStats.connections`는 누적 수이므로 현재 연결 수로 해석하지 않습니다.

Netem은 Host A의 private `lan0` egress에만 적용합니다.

| 주입 | 적용 범위 | 판정 |
| --- | --- | --- |
| loss 5% | UDP destination port 37652만 | missed 증가, 다음 sample 지속, TCP Get 정상 |
| delay 70±10 ms + loss 5% + duplicate 30% + reorder 50%/correlation 25% | UDP 37652만 | duplicate/out-of-order 통계 증가와 최신값 지속 |
| delay 40±10 ms | A의 전체 egress | Control/Topic 동시 정상 처리, slow Service/Action timeout의 execution_unknown 유지 |

UDP 전용 주입은 `prio` qdisc와 u32 UDP/port filter로 분리하므로 Discovery/Control은 해당 loss 필터의 대상이 아닙니다. 정확한 loss 개수나 latency를 고정하지 않습니다. 실제 관측된 통계와 qdisc 설정은 결과에 남깁니다. 짧은 functional deadline을 가진 synthetic slow operation은 처리 여부가 불확실한 timeout을 의도적으로 만들고, 이후 Local handler/Goal 실행 횟수가 한 번인지 검사해 자동 retry가 없음을 확인합니다.

Controller의 12~20초 wait 상한은 hang 방지용입니다. 일반 Remote 요청 timeout은 3초이며 exact wall-clock duration을 assertion하지 않습니다. Rate 검사는 충분한 관측 구간과 여유를 두고 Local이 Remote보다 빠르며 50 Hz 정책에 근접하는지 확인합니다. 이 시험은 hard real-time latency 보장이 아닙니다.

## 최초 Gateway 재시작과 Local SHM 제한 (2026-09-28 historical)

수신 Gateway B가 SIGKILL되면 `/dev/shm/virtual%2Fa%2Ftopic` proxy 이름과 기존 Local Subscriber mapping이 남습니다. 새 Gateway B는 기존 proxy 이름을 exclusive create할 수 없어 `-EEXIST`를 보고하며 payload receive route를 만들지 않습니다. Discovery는 ONLINE으로 복구하므로 이것은 Network discovery 실패가 아니라 **기존 Local SHM 소유권/수명 정책**의 제한입니다.

시험은 남은 SHM 목록, 새 Gateway의 conflict 진단과 기존 Subscriber 상태를 기록합니다. 이후 기존 Subscriber와 Gateway를 종료하고 B의 **해당 proxy object 하나만** 명시적으로 삭제한 뒤 Gateway/Subscriber를 다시 생성합니다. Topic과 Control이 복구되는지 검증합니다. 자동 reclaim/reconnect, ABI 변경 또는 Framework 수정은 하지 않습니다.

송신 Gateway A를 재시작할 때도 사용하지 않는 반대 방향 bootstrap proxy 이름이 남을 수 있습니다. 실제 수신을 재개하려는 방향에서는 동일한 명시적 정리 정책이 필요합니다.

## 최초 검증 결과 (2026-09-28 historical)

아래는 자동 ownership recovery 추가 전 기록입니다. 현재 구현과 후속 실행 결과는 다음 절에 별도로 기록합니다. 기존 localhost/Local IPC 검증이나 실제 물리 LAN 검증과 구분합니다.

2026-09-28 Debug C++17 실행: **Virtual LAN PASS**, 알려진 제한 2개는 별도 LIMITATION으로 유지합니다. 결과는 `build/network-virtual/final/results.json`, 세부 명령/관측은 `events.jsonl`에 있습니다.

| 항목 | 결과 | 관측 |
| --- | --- | --- |
| Namespace / Local IPC 격리 | PASS | net/ipc/mount namespace 분리, 양쪽 registry/SHM 분리, cross-host Topic/Parameter Open=-ENOENT |
| Discovery v2 | PASS | lan0 multicast, 양방향 ONLINE, self 제외, 실제 10.203.0.x 주소와 metadata/capability |
| Remote Parameter | PASS | Get/Set, owner Local 값 변경, LOCAL-only/type 거부, 역방향 Get |
| Remote Service | PASS | 실제 Local handler와 response 확인 |
| Remote Action | PASS | accepted→여러 feedback→terminal result; 별도 Cancel ack/terminal 취소 |
| Remote Topic | PASS | Subscriber 전 무전송, proxy Local Subscriber 수신, sequence 중복 전송 없음 |
| Control + Topic | PASS | 고주기 발행 중 Get/Set, Service, Goal/Cancel |
| Peer SIGKILL / restart | PASS | LOST/route 제거, Local owner 지속, 새 session/route, Remote operation 복구 |
| Interface down/up | PASS | 실제 lan0 down, LOST/TCP 해제, up 후 rediscovery |
| UDP loss 5% | PASS | missed +11, 다음 sample 지속, TCP Get 정상 |
| UDP duplicate/reorder | PASS | missed +106, duplicate +28, out_of_order +143; 지속 수신 |
| Delay/jitter / timeout | PASS | 40±10 ms, execution_unknown 유지, Local 실행 횟수로 자동 retry 없음 확인 |
| Gateway process / SHM restart | LIMITATION | ONLINE 복귀하지만 기존 proxy 이름 때문에 EEXIST/수신 route 0 |
| 명시적 SHM 정리·재연결 | PASS | 기존 reader/Gateway 종료→proxy 하나 unlink→새 Gateway/Subscriber 정상 |
| Cleanup | PASS | 정상 종료 및 개발 중 실패 경로에서 child/namespace 정리 |
| 실제 물리 LAN | LIMITATION | 같은 kernel/CPU의 virtual NIC; 물리 NIC/switch/Wi-Fi, 이기종 장치·clock 및 실제 배선 미검증 |

Local 발행 측정값은 **199.03 Hz**, Remote는 **46.89 Hz**였습니다. Netem 통계는 해당 실행의 관측값이며 고정 기대값이 아닙니다.

수신 Gateway 재시작 시 남은 `virtual%2Fa%2Ftopic`과 기존 Subscriber의 registry/mapping을 결과에 기록했습니다. 새 Gateway는 conflict를 보고했고, 기존 reader의 신규 Open/Read 관측은 -EAGAIN(-11)이었습니다. 어떤 반환 경로에서 실패했는지는 통합 read 관측만으로 더 세분화하지 않습니다. 명시적 cleanup 후 정상 복구되어 Network wire 변경 없이 Local SHM 제한을 재현했습니다.

Fixture 개발 중 세 실패를 수정했습니다. 누적 `ControlStats.connections`를 active 수로 해석한 검사는 `ss` 기반으로 바꿨습니다. ACCEPTED 단계에서 Cancel이 먼저 도착하면 Start가 -EINVAL을 반환하는 정상 계약을 fixture가 처리하도록 수정했습니다. Gateway kill **이전** callback 수와 비교하던 검사는 writer 종료와 proxy conflict 확인 **이후** 안정된 수신 수를 비교하도록 고쳤습니다. Framework timeout이나 protocol을 완화하지 않았습니다.

이 작업의 Framework 기능/ABI/storage 코드 변경은 없습니다. 새 결함으로 확정한 Framework 문제는 없으며 위 SHM 재시작 제약을 유지했습니다. 기존 Local IPC 11개와 sanitizer 결과는 이전 단계 기록으로 남기고 이번 Virtual LAN 결과와 혼합하지 않습니다.

별도로 최종 순차 실행한 기존 localhost Network 7개 regression도 PASS였습니다: Network contract, compatibility, Discovery, Remote Parameter, Remote Service, Remote Action, Remote Topic Data Plane. 로그는 `build/network-virtual/final/regression/`에 있습니다. 새 fixture Debug C++17 build는 경고 없이 PASS이며 Bash/Python 문법, 문서 로컬 링크와 `git diff --check`도 통과했습니다. Commit/push는 수행하지 않았습니다.


## Network proxy 자동 복구 재검증

2026-09-29부터 runner는 수신 Gateway를 SIGKILL하고, 기존 Subscriber를 종료한 뒤 새 Gateway가 sidecar로 stale proxy를 자동 복구하는지 확인합니다. `proxy_recovered=1`, `proxy_stale_detected=1`, 새 local discovery session과 route, Subscriber 재생성 후 정상 payload 수신을 요구합니다. **수동 shm unlink 단계는 없습니다.** Source owner/peer는 계속 동작합니다.

이어 정상 Gateway process 종료에서 owned proxy 이름이 정리되는지 확인하고, 새 Gateway/Subscriber에서 재수신합니다. 정상 Stop만 호출한 같은 객체의 mapping 유지와 process 소멸의 cleanup은 구분합니다.

기존 수동 SHM cleanup 제한은 유효한 ownership sidecar가 있는 Network proxy에 대해 해소되었습니다. Format 4 ABI나 일반 Local Publisher 정책은 바꾸지 않았습니다. 이전 inode에 붙은 Local Subscriber는 여전히 명시적으로 재연결하며, sidecar 없는 legacy/unknown/corrupt object는 자동 복구하지 않습니다. 상세 조건은 [Network proxy recovery 계약](08_NETWORK_ARCHITECTURE.md#network-proxy-stale-shm-recovery)을 따릅니다.

실행: `./tests/network_virtual/run_virtual_network_test.sh build build/network-virtual/proxy-recovery`

| 항목 | 결과 |
| --- | --- |
| Namespace/IPC 격리와 Discovery v2 | PASS |
| Parameter/Service/Action/Topic 및 동시 부하 | PASS |
| Peer kill/restart, interface down/up | PASS |
| UDP loss/duplicate/reorder, delay/jitter와 execution_unknown | PASS |
| Receiver Gateway SIGKILL → 자동 stale proxy recovery | PASS — manual_unlink=false, proxy_recovered=1 |
| 새 Local Subscriber 연결 → Topic route/수신 | PASS |
| 정상 Gateway 소멸 → proxy cleanup → 재시작 수신 | PASS |
| 실제 물리 LAN | LIMITATION — 미검증 유지 |

결과는 `build/network-virtual/proxy-recovery/results.json`, raw 관측은 같은 디렉터리 `events.jsonl`에 있습니다. 최초 historical 결과의 `gateway_shm_restart_limit`/`explicit_shm_cleanup_reconnect`는 현재 runner에서 `gateway_shm_auto_recovery`/`gateway_normal_restart`로 대체되었습니다.

추가 복구 unit/통합 테스트와 기존 Network·Local IPC·ASan/UBSan 결과는 [proxy recovery 검증 기록](08_NETWORK_ARCHITECTURE.md#proxy-recovery-검증-기록-2026-09-29)을 참고하세요. 기존 Action/Topic 회귀의 최초 대기 실패와 단독 재실행 PASS 이력도 별도로 보존했습니다.
