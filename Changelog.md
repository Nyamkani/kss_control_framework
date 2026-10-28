# Changelog

KSS Control Framework(KCF)의 주요 개발 및 검증 이력입니다.
기존 Phase 1-1~1-7의 v0.1~v0.7 이력은 유지하고 Phase 1 완료 기준을 v1.0으로 기록합니다.
Phase 2 통신 기능은 v2.0, Phase 3 supervision/recovery는 v3.0에 기록합니다.
버전은 최신순으로, 각 버전 내부는 Phase 진행 순서대로 정리합니다.
이 문서의 버전 표기는 개발 이력 구분이며 Git tag 또는 배포 생성 여부를 의미하지 않습니다.
v1.0~v4.3은 historical record입니다. 당시 Mecanum 경로·실행 범위를 현재 Framework 소스 구성으로 해석하지 않습니다.

## v5.6 — Framework-managed Network / LAN Validation

2026-10-28: CMake 및 기본 FrameworkVersion을 **5.6.0**으로 정합화하고 Discovery Framework minor 기대값을 6으로 갱신.
정식 Release·Tag·배포 완료를 의미하지 않는다. NetworkProtocol 1.0 / Discovery v2 / Control TCP v1 / Topic wire v1 및 IPC 계약 유지.

- 영구 Host Identity, NIC Resolver, 독립 Gateway/systemd 템플릿, Network lifecycle 재바인딩.
- Topic Exposure/읽기 전용 Observer, Local Gateway IPC, Topic별 Rate 및 절대 주기 스케줄러.
- Fresh-start 및 수신 Commit 기반 CommandFreshnessMonitor. 실제 Fail-safe는 Application 책임.
- 원격 Parameter/Service/Action 노출·독립 Host 권한 CLI, Action 예제 TypeDescriptorTraits.
- Tools v0.14.0 연동 OPI5+ 사용자 LAN 시험: Local Publish 약 200Hz / Busy 0, 유선 50/100/200Hz 목표 근접,
  Wi-Fi 각각 48~52 / 99~102 / 199~205Hz로 수신률 1차 PASS. 200Hz Publisher는 장치 로컬 변경이며 기본 1Hz 예제의 지원 선언이 아님.
- Wi-Fi Discovery ONLINE/LOST·멀티캐스트 공백 재현. 수동 IGMPv3 Report만으로 복구되나 정확한 원인은 미확정.
  Graph는 자동 갱신되지 않아 이전 Snapshot이 남을 수 있음.
- UDP 손실률·p95/p99 지연·장시간 안정성·실제 systemd 부팅/자동 재시작·RT·로봇 Fail-safe 미검증. 서비스 비활성 유지.
  [사용자 보고 및 한계](docs/06_VERIFICATION_AND_LIMITATIONS.md#net56-lan-20261028).

## v5.5 — Supervisor Lifecycle / Registration / Ownership Contract

### S1 — Lifecycle 및 Error Management

- 초기 Managed 실행/Setup/barrier 실패도 Application ERROR로 유지하며 정상 자식을 계속 감시.
  Supervisor 준비가 완료되면 Setup은 0을 반환하고 Run 진입 가능; RUNNING 보장과 구분.
- 최초 Current Error와 최근 64건의 내부 Error History를 분리. 발생 순서·Monotonic 시각·generation·
  Element/PID·failure_kind·Runtime 오류 관측 여부·exit 정보를 기록하고 반복 폴링을 중복 기록하지 않음.
- 명시적 RequestReset만 전체 Managed generation을 종료·회수 후 재실행.
  성공 시 Current Error 해제, History 유지. 자동 Reset/Restart 없음.
- RequestShutdown 및 SIGINT/SIGTERM의 정상 종료와 Reset 중 Shutdown 우선 처리.
  자식 회수 후 STOPPED 게시; 기존 enum 숫자값을 유지하도록 마지막에 추가.

### S2 — Element 등록 사전 검증

- static Bringup::ValidateElements와 선택적 RegistrationError(index/name/field/reason) 추가.
  빈 목록은 index 없음, 성공 시 이전 오류 출력 초기화, 최초 형식 오류는 -EINVAL.
- Setup도 동일 검증을 사용하며 lifecycle -EBUSY 검사를 유지. 기존 ElementSpec aggregate 및 Setup 호출 호환.
- 실행 파일·RT 권한·실제 Element Setup 성공 검사는 아님. enabled/동적 등록/의존성 그래프 추가 없음.

### S3 — Managed / External 책임 문서화

- Supervisor는 Managed lifecycle만 소유. External은 독립 실행하며 장애를 자동 전파하지 않음.
  IPC 공급 중단과 SHM 세대 변경은 외부 소비자에 영향을 줄 수 있음을 명시.
- Attached는 미구현·보류. Builder의 Managed 실행 목록과 External 정적 참조를 구분하며 도구·파서는 구현하지 않음.
- [Runtime 계약](docs/02_RUNTIME_AND_SUPERVISION.md)과
  [Application 가이드](docs/05_APPLICATION_DEVELOPMENT_GUIDE.md)를 현재 API에 맞춰 정리.

### S4 — 버전 정합화 및 통합 검증

- CMake project와 기본 FrameworkVersion을 5.5.0으로 정합화.
  IPC/storage 및 NetworkProtocol 1.0, Discovery v2, Control TCP v1, Topic wire v1은 변경하지 않음.
  NETWORK_PROTOCOL 정책 유지; EXACT_FRAMEWORK_VERSION은 v5.4와 v5.5를 구분.
- Debug 전체 configure/build PASS. 최초 28개 실행 항목은 27 PASS / Discovery 버전 기대값 1 FAIL.
  광고 minor 기대값 4→5 보정 후 해당 target 재빌드·단독 재실행 PASS; 최초 FAIL 이력 유지.
  RemoteTopicClient는 이번 1회 PASS이나 기존 Known Intermittent Failure는 미해결로 유지.
- 실제 최초 실행 결과와 제한은 [v5.5 S4 검증](docs/06_VERIFICATION_AND_LIMITATIONS.md#v55-s4-integration)에 기록.
  과거 v5.4 Network Timeout, 물리 LAN Discovery 및 PREEMPT_RT 미검증 이력은 보존.
- README는 이번 범위에서 수정하지 않음. 이 개발 기록은 commit/push/tag/Release 생성을 의미하지 않음.

## v5.4 — Generic Remote Control LAN Validation / Network Deployment Policy

- CMake project version과 기본 FrameworkVersion을 5.4.0으로 정합화. KCF Tools 연동 기준은 v0.14.
- 기존 RemoteControlClient의 Parameter Get/Set/Readback, Service Call, Action Goal/Feedback/Result/Cancel과
  TypeDescriptor Query, 정확한 Endpoint 선택 기반 RemoteTopicClient 및 Multi-Host UI 연동을 정리.
- NetworkProtocolVersion 1.0, Discovery v2, Control TCP v1, Topic wire v1 및 Local IPC storage/protocol 유지.
  NETWORK_PROTOCOL 정책은 그대로이며 EXACT_FRAMEWORK_VERSION에서 v5.3과 v5.4는 불일치.
- 다중 장치 공통 Gateway, 고유 Host Identity, TCP/UDP 및 동적 Client 포트 정책을
  [Deployment Policy v1.0](docs/10_NETWORK_DEPLOYMENT_POLICY.md)에 문서화.
  39001 기본 포트 자동 적용, 영구 Host ID provisioning 및 Debian/systemd 패키징은 여전히 미구현 설계안.

### 물리 LAN 1차 기능 검증 — 이전 사용자 보고

2026-10-21 Orange Pi 5+ ↔ Ubuntu VM Bridged LAN에서 Framework dev `9e9cc1b`(v5.3),
Tools main `e332438`(v0.13)로 수행한 사용자 수동 기록이다. v5.4/Tool v0.14 재실행 결과가 아니다.

- Discovery/Metadata, Remote Topic 실제 수신(약 45.96 Hz), Parameter Get/Set/Readback,
  Service Call/Response, Action Goal/Feedback/SUCCEEDED 및 Cancel ACK/CANCELED: 사용자 확인 PASS.
- Cancel Result: count=-1, enabled=false, gain=1.5. 최종 execution_unknown=0, cancel_unknown=0.
- 공개 Action 예제는 2초 완료 설정이며 Cancel 수동 시험은 Orange Pi에서 10초로 변경해 수행.
- UFW Discovery 차단, 동일 Host ID 중복 Gateway, 일시적 장애 후 양쪽 프로그램 재시작 사례를 보존.
- Gateway 단독 재시작, Tools 단독 재시작, LAN 장애 후 자동 복구와 다중 Gateway 운영 안정성은 미검증.
  기존 Action 자동 reattach 및 변경 명령 자동 retry는 미지원.
- [상세 시험 기록](examples/network/LAN_TEST_GUIDE.md#physical-lan-2026-10-21).
  1차 기능 PASS는 전체 네트워크 안정성 검증 완료를 뜻하지 않는다.

### v5.4.0 version alignment 검증

- CMake Debug configure 및 전체 build: PASS. CMake/기본 FrameworkVersion 5.4.0 일치.
- 격리 loopback/SHM 환경에서 직렬 Network 회귀 10/10 PASS:
  Network contract, Compatibility, PeerDiscovery, Parameter, Service, Action,
  Topic Data Plane, Proxy Recovery, RemoteControlClient, RemoteTopicClient.
- 이번 1회 실행에서는 실패·재실행 없음. 과거 간헐적 실패가 해소됐다는 의미는 아님.
- 물리 LAN 및 KCF Tools v0.14 연동 재검증: 미실행.
- v5.4 버전 정합화 검증 당시에는 Commit/Push를 수행하지 않았으며, 이후 해당 변경 사항을 
  dev 브랜치에 반영했다. 기존 v5.3 이하 검증 이력은 보존한다.

### Supervisor ExecutionProfile — v5.4 로컬 개발 변경

- `ExecutionProfile`: INHERIT/OTHER/FIFO/RR, priority, cpu_affinity, lock_memory(false).
  Process::Start의 선택적 네 번째 인자와 ElementSpec의 마지막 멤버로 전달하며 Reset에도 재적용.
- fork 전 검증, exec 전 affinity/scheduler 적용. 실패 시 `-errno`, exec 중단 및 child 회수.
- exec 이후 Runtime 초기화와 Element Setup 사이에 opt-in mlockall 적용.
  내부 환경변수 검증/소비/제거, 부모 값 대체, 실패 시 Element 미실행, 종료 시 획득 잠금 정리.

### 3A / 3B 1kHz Benchmark와 주기 복구

- Runtime 단독 및 별도 Publisher/Subscriber SHM Topic Benchmark 추가. 기존 Process/Supervision 사용,
  기본 60초/5초 Warm-up, 정상 종료·자식/FD/SHM 회수 확인.
- 3A `--trace-outliers`: 기본 OFF, 사전 할당 기록, 종료 후 상위 20개/전후 5개 및 임계값별 집계.
- 지연 시 Runtime을 `next_tick = now + period`로 재예약하고 대기하도록 변경.
  정상 절대 시각 스케줄링, Loop 오류/heartbeat/Stop/Shutdown 계약은 유지.
- VM에서 Trace ON A/B/C 각 3회와 수정 후 각 1회 기록을 구분. 수정 후 확인한 상위 60개
  지연 직후 15–20µs 연속 실행은 없었으나 평균 Hz는 낮아질 수 있음. hard real-time 보장이 아님.
  초기 B의 680.441ms 이상치는 후속 9회에서 미재현이며 원인 미확정.

### Application Composition / External Observer

- Source 100Hz → Processor 50Hz → 독립 Observer 10Hz. 원본 sequence/timestamp 보존,
  latest-value skip 및 Age/STALE 관측. Source/Processor만 Bringup 관리, Observer 소유권 추가 없음.
- 기존 ERROR 정책 및 수동 재시작 유지. 정상 종료의 소유 SHM 정리와 Managed 자식 회수 검증.
- Observer `r` + Enter로 동일 PID에서 명시적 Close → 세대별 Snapshot/비교 상태 초기화 → Create 1회.
  실패 시 DISCONNECTED로 이전 데이터를 폐기하고 다음 사용자 요청 대기. 자동 reconnect/retry 없음.
- 실행 방법: [Composition README](examples/application_composition/README.md).

### 로컬 통합 검증 — 2026-10-22

이번 통합 단계는 미커밋 working tree 기준이며 제품 기능을 추가 수정하지 않습니다.
전체 Debug 빌드와 실제 회귀 실행 결과는 [v5.4 통합 검증](docs/06_VERIFICATION_AND_LIMITATIONS.md#v54-local-integration)에 기록합니다.
최초 26개 실행 항목 중 25 PASS / RemoteTopicClient peer 대기 timeout 1 FAIL, 해당 항목 단독 재실행 PASS.
원인은 미확정이며 최초 전체 PASS로 소급하지 않습니다.
후속 동일 테스트 5회에서는 2회 동일 Timeout을 재현했고, 테스트 진단 출력 추가 후에는 Debug 빌드 및 10/10 PASS를 기록했습니다.
두 Host의 ONLINE/Metadata/Endpoint 준비는 정상 관측됐지만 진단 출력의 타이밍 영향 가능성이 있으며,
Timeout 진단 실패 분기는 아직 실행 검증되지 않았습니다.
**Known Intermittent Failure**로 유지하고 추후 실패 로그 확보 시 재조사합니다.
v5.4 기능 개발 완료와 전체 회귀 무실패 판정은 구분하며, 후자는 계속 보류합니다.
이 후속 결과는 기존 실행 기록의 문서 반영이며 이번 문서 작업에서 테스트를 추가 실행하지 않았습니다.
기존 1kHz 성능값은 이전 실행 기록이며 이번 문서 정리에서 재측정하지 않습니다.
PREEMPT_RT 실장 검증은 미수행, 물리 LAN Discovery 지연과 장애 후 복구 안정성은 미해결/미검증으로 유지합니다.
버전은 5.4.0을 유지하며 commit/push/tag/Release는 생성하지 않습니다.

## v5.3 — Read-only Network Diagnostics

F-ND1~F-ND5A가 적용된 dev working tree 기준입니다. FrameworkVersion/CMake는 5.3.0이며,
공개 commit/tag/Release 또는 Tool v0.13 배포를 의미하지 않습니다. v5.2와 Tool v0.12의 아래 기록은 당시 이력으로 보존합니다.

### F-ND1 — Snapshot codec

- Gateway Host/boot/session과 route identity/state/reason/recovery 및 9개 uint64 counter의 read-only snapshot.
- `MakeNetworkDiagnosticsSnapshot`, `EncodeNetworkDiagnosticsSnapshot`, `DecodeNetworkDiagnosticsSnapshot` 추가.
- Diagnostics payload v1, 16 KiB 상한, `total_routes`/`truncated`, 잘린·잘못된 입력 거부. Topic payload/전체 proxy 목록은 포함하지 않음.

### F-ND2A / F-ND2B — Control Protocol / Gateway Handler

- 기존 Control TCP wire v1에 DIAGNOSTICS_REQUEST(14)/RESPONSE(15)와 HOST 조회 계약 추가.
- `diagnostics_enabled=false` 기본값 유지. 명시적 opt-in 및 시작 준비 후에만 NETWORK_DIAGNOSTICS 광고.
- Discovery-only Observer의 read-only HELLO/identity/session gate, bounded 연결·job·응답 및 rate limit.
- 기존 Parameter/Service/Action 실행 경로와 분리된 Diagnostics dispatch/replay 처리.

### F-ND3 — Client API

- 실행 중인 PeerDiscovery를 재사용하는 `NetworkDiagnosticsClient::Query` 공개.
- 동기식 worker API, 단일 deadline, Query마다 TCP 생성·정리, 자동 retry 없음, 실패 시 출력 불변.
- Host/boot/session 및 request correlation 재검증, EOF/partial IO/timeout/stale 처리.

### F-ND4A — Observer Replay Lifecycle

- Observer replay 기록은 최대 64개. 동일 incarnation이 ONLINE이거나 연결이 남아 있으면 유지.
- LOST/소멸/교체된 incarnation이며 연결도 없는 경우에만 회수. 회수 후 같은 session 재등장도 새 read-only replay window.
- 일반 Gateway Diagnostics 및 Parameter/Service/Action replay 상태는 이 회수 정책의 대상이 아님.

### F-ND4B — Diagnostics E2E

- 실제 두 DataPlane/proxy Subscriber, public client 조회, route/session/counter 상관관계 및 Control 동시 부하 검증.
- Local 약 200 Hz와 Network 약 50 Hz, skipped counter를 분리 관측. Proxy Publish는 Subscriber 처리율이 아님.
- 별도 Tool Virtual LAN E2E와 Framework 자체 시험을 분리 기록. 최초 timeout·재실행 이력을 아래 검증 문서에 보존.

### F-ND5A — Version Alignment / Header Cleanup

- CMake 및 `FrameworkVersion` 기본값을 5.3.0으로 통일, 오래된 헤더 주석 3곳 수정.
- Discovery 테스트의 Framework minor 기대값만 2→3 변경. API/동작/호환성 정책 및 protocol/storage version 유지.
- 전체 Debug build, Network 8/8, Diagnostics Loopback E2E, Framework Virtual LAN PASS.

F-ND5B는 위 결과를 문서화하며 새 build/test를 실행하지 않았습니다.
[공개 API·계약](docs/08_NETWORK_ARCHITECTURE.md#network-diagnostics-v53),
[출처별 검증·간헐적 실패·남은 제한](docs/06_VERIFICATION_AND_LIMITATIONS.md#v53-network-diagnostics-검증)을 참고하세요.

## v5.2 — Network IPC / Remote Discovery·Control·Topic

KCF v5.2의 5.2.0 version-alignment commit은
[`d41fc2fc71d3b60b0475035ec56ac32b92ffb849`](https://github.com/Nyamkani/kss_control_framework/commit/d41fc2fc71d3b60b0475035ec56ac32b92ffb849)입니다.
Network 기능 최초 공개는 dev commit
[`0a3f2a6`](https://github.com/Nyamkani/kss_control_framework/commit/0a3f2a630cec209793f9ac6e99107752e9e01283)입니다.
이전 v5.1 기준은 `8360b6e053472ba20acdfc62b7489e33122997ec`이며 Topic Queue 및 Local IPC 계약은 유지합니다.

Historical: 최초 v5.2 문서 정리 당시에는 working tree를 기준으로 기록했고 commit/push·기능 테스트 재실행을 하지 않았습니다.
당시 CMake/Network 광고 기본값은 `5.1.0`이었습니다. 이후 위 공개 commit 반영을 확인하고,
후속 보정에서 두 기본값을 `5.2.0`으로 일치시켰으며, 이 version/document alignment 변경은 이후 `d41fc2f`로 dev에 반영되었습니다. Protocol/storage version과 compatibility 정책은 그대로입니다.
Discovery codec 테스트의 Framework minor 기대 byte만 1→2로 정정했습니다. 전체 빌드와 compatibility/contract 및 Network 회귀 결과는 아래 링크의 후속 기록을 따릅니다.
과거 단계별 검증 기록은 아래에 보존하며 후속 실행 결과는 [검증 문서](docs/06_VERIFICATION_AND_LIMITATIONS.md)에 별도로 기록합니다.
Git tag/GitHub Release 여부는 이번에 확인하지 않았습니다.

### 1. Network 공통 모델

- Local-first / 중앙 Master 없는 peer-to-peer 구조, Host·boot·Runtime·Endpoint identity 정의.
- LOCAL 기본 / REMOTE opt-in, CONTROL(HIGH)과 TOPIC(NORMAL) 분리 및 bounded resource 계약.
- ENDPOINT / HOST / APPLICATION / GROUP / ALL target, endpoint별 결과·timeout·부분 실패 모델 추가.
- type/schema/encoding/size 검증과 operation tag codec 추가. Payload 측정 Sequence·Timestamp·Validity는 Application 책임 유지.

### 2. Peer Discovery 및 Compatibility

- IPv4 UDP multicast announce와 필요 시 unicast metadata 교환, process-local Peer Table 구현.
- ONLINE/LOST, announce timeout, metadata revision, boot/session 변경, 자기 자신 제외와 Start/Stop 정리.
- Discovery v2에 Framework/Network version 및 capability 포함. peer 호환성과 endpoint type·scope·target 호환성을 구분.
- malformed/이전 wire version 거부, metadata 상한 및 재조립 검증. 이름이나 IP 대신 전체 identity 사용.

### 3. Local metadata 자동 연동 / Control Transport / Remote Parameter

- 기존 introspection에서 Application·Element·T/P/S/A metadata를 수집하고 변경·삭제를 revision에 반영.
- Control TCP v1, bounded frame/queue, request correlation, timeout/disconnect 및 중복 실행 방지 처리.
- 명시적 exposure와 호환성 검사 후 기존 Local Parameter Get/Set 호출. 5종 Target과 endpoint별 fan-out 결과 제공.
- Network worker와 Local Runtime/IPC를 분리하고 자동 request retry 없이 실행 불확실성을 전달.

### 4. Remote Service

- 공통 Control Transport를 통해 기존 Local Service handler를 호출하고 request/response type 검증.
- Target/fan-out, endpoint별 성공·실패·timeout 및 `execution_unknown` 처리.
- Network 실패를 Local handler 미실행이나 rollback으로 해석하지 않으며 자동 재호출하지 않음.

### 5. Remote Action

- Local Action introspection과 DynamicActionClient를 연결하고 Goal/Feedback/Result/Cancel 구현.
- Feedback은 bounded latest, terminal Result는 별도 전달. Cancel 수락과 실제 CANCELED를 구분.
- 연결 소실 시 Local Goal을 자동 취소하지 않음. 자동 Goal/Cancel retry 및 기존 Goal re-attach는 미지원.

### 6. Remote Topic Data Plane

- Control과 분리한 UDP unicast Data Plane 및 Topic wire v1 구현.
- `remote_topics` allowlist, 호환 type과 실제 Subscriber를 확인한 route만 생성.
- Best-effort latest-data, 기본 최대 50 Hz, 재전송 없음. Datagram/payload 상한과 큰 payload 거부.
- 수신 Gateway의 native TypeDescriptor catalog로 Local proxy를 bootstrap하고 기존 Subscriber API 사용.
- route 및 sent/received/drop/duplicate/out-of-order/missed/error 진단을 Gateway 객체에서 제공.

### 7. 격리된 Virtual 2-Host LAN

- 독립 network/IPC/mount namespace, 각 Host의 private `/dev/shm`, 실제 veth/bridge fixture 추가.
- Discovery·Parameter·Service·Action·Topic 및 동시 부하, peer SIGKILL/restart, NIC down/up 검증.
- Netem loss/delay/jitter/duplicate/reorder와 timeout 불확실성 검증. 일반 회귀와 분리된 opt-in runner 제공.
- 최초 stale proxy의 수동 정리 제한은 historical record로 보존하고 다음 복구 단계의 결과와 구분.

### 8. Network proxy stale SHM 안전 복구

- Network 전용 ownership sidecar v1로 owner Host/boot/PID/start_ticks, source 및 Topic/type/inode 관계 기록.
- 죽은 Network Gateway 소유임이 증명된 proxy만 자동 복구. Local Publisher, live owner, unknown/corrupt object는 보호.
- 정상 소멸 cleanup, SIGKILL 후 복구, orphan sidecar 및 동시 복구 검증.
- 기존 Local Subscriber mapping은 자동 교체하지 않으므로 명시적 Close/Create 또는 process 재시작 필요.
- Local Topic Format 4 / Parameter Format 3 / Service protocol 2 유지. Discovery v2 / Control TCP v1 / Topic wire v1 유지.

### 9. kcf_tools read-only 관측 연동 — 별도 저장소

Historical 구현 참조: kcf_tools **v0.12** 최초 기능 반영은 GitHub main commit
[`f4fb447`](https://github.com/Nyamkani/kcf_tools/commit/f4fb4471fc0b03b24663eb20eede8d8491fd87f6)입니다. 현재 Tool HEAD를 나타내는 기록은 아닙니다.

- 공개 PeerDiscovery API를 이용한 background 관측과 수동 Refresh snapshot, Local/Remote identity 분리.
- Host → Application → Element → T/P/S/A metadata Graph, ONLINE/LOST, version·compatibility·capability 상세.
- Host/Application 필터, Local/Remote·T/P/S/A toggle, Zoom/Pan 및 읽기 전용 Remote Detail.
- Parameter/Service/Action은 발견된 소유 관계만 표시. Remote endpoint를 Local 조작 화면으로 연결하지 않음.
- **외부 Gateway active route/statistics·proxy 판별은 미완료**: `GetTopicDiagnostics()`가 process-local 객체 API이고,
  Discovery에는 타 Gateway의 진단 조회 경로가 없음. 같은 이름/type만으로 cross-host edge를 생성하지 않음.
- Tool은 forwarding/Gateway가 아니며 Remote Set/Call/Goal/Cancel UI는 추가하지 않음. Tool 버전은 Framework v5.2와 별도.

### 검증 기록과 남은 제한

아래는 구현 단계에서 실제 수행하고 기록한 결과의 요약입니다. **당시 v5.2 문서 정리에서는 build/test를 재실행하지 않았습니다.** 후속 버전 보정의 검증은 별도 기록입니다.

- C++17 전체 Debug build 및 새 Network 코드 warning 검사 PASS.
- Network contract/compatibility/Discovery/Parameter/Service/Action/Topic 7개 회귀와 proxy recovery 검증 PASS 기록.
  Action·Topic에서 최초 대기 assertion 실패 후 단독/후속 재실행 PASS한 이력이 있으며 원인은 확정하지 않음.
- 기존 Local IPC 11개 회귀 PASS. Proxy recovery 및 Remote Topic ASan/UBSan PASS.
- Virtual LAN 전체 재검증 PASS: 수동 shm unlink 없이 SIGKILL → proxy recovery → 새 Subscriber 연결 → 수신 재개.
- 별도 Tool 저장소: KCF+Qt 18/18, Framework 없는 Mock/stub+Qt 6/6 PASS. 후속 상세 선택 해제 보완 후 전체 회귀도 PASS.
- Framework Virtual LAN PASS와 Tool의 loopback/GUI 검증은 별개. Tool+Virtual LAN 통합 및 실제 물리 LAN은 미검증.
- Reliable Topic QoS, 범용 schema/codec 교환, 자동 request retry, Action re-attach, 인증·권한·암호화는 미지원.
  기존 단일 Publisher, dead-reader pin, SHM 재생성 후 명시적 재연결 제한은 유지.

세부 API·단계별 검증은 [Network architecture](docs/08_NETWORK_ARCHITECTURE.md),
[Virtual LAN 검증](docs/09_VIRTUAL_LAN_TEST.md)에 기록합니다.

## v5.1 — Topic Queue

- Depth 기반 Bounded Ring Buffer 추가: 기본 Depth=1 Snapshot, Depth=N KEEP_LAST.
- Subscriber별 독립 Cursor, NEXT/OLDEST, ReadNext 및 누락 검출. ReadLatest/기존 Callback은 최신값 계약 유지.
- Transport Sequence 64-bit, Commit 성공 시 증가. -EAGAIN 실패 시 Queue/Sequence 및 읽기 Cursor 유지.
- Payload 측정 Sequence·Timestamp·Validity는 Application 책임이며 valid=false도 변경 없이 전달.
- Topic SHM Format 4로 변경, Parameter Format 3 유지. 기존 Topic 바이너리 재빌드 필요; 자동 SHM migration 없음.
- pin/CAS·복구 보호, 단일 Publisher, dead-reader pin 및 명시적 재연결 제한 유지.
- 기존 v5.0 구현·검증 기록은 [별도 이력](docs/V5_0_STATUS.md)으로 보존.
- 최종 실제 실행: C++17 전체 빌드, Queue 10개 영역, Framework 회귀 22/22 (19+3), Tool Backend/Echo 1/1 PASS.
- GitHub dev의 [82fa3b2](https://github.com/Nyamkani/kss_control_framework/commit/82fa3b274f51d847602b2a5e4936778ebd631257)로 v5.1 코드·문서 반영 확인. tag/공개 Release 상태는 [v5.1 상태](docs/V5_1_STATUS.md) 참조.
- README 통합: 현재 진입 문서는 README.md 하나로 정리. 유효한 사용법·과거 검증 요약은 docs에 보존하고 구 Readme.md 삭제. 통합 작업에서는 기능 테스트를 재실행하지 않음.

## v4.3 — Pre-Application Execution / Concurrency Base

- ExecutionMode(STANDALONE/SUPERVISED)와 header-only DetectLaunchExecutionMode 추가. 기존 환경변수 상수를 재사용하고 Run 전에 존재 여부만 확인.
- 실제 supervision FD 검증·Runtime scheduling·worker 동작은 유지. 잘못된 환경변수도 mode는 SUPERVISED이며 Runtime은 Setup 전에 실패.
- Application 전용 common/time.hpp 추가: CLOCK_MONOTONIC microsecond 생성 및 -errno 반환, pure IsTimestampFresh helper 제공.
- Freshness는 0/future timestamp 거부 및 max_age 경계 포함. 고정 timeout/retry 정책 없음.
- Callback과 Loop의 동시 실행, 짧은 Application mutex 아래 snapshot 복사, unlock 후 Loop에서 제어·I/O 수행하는 계약 문서화.
- v4.2 transient/fatal error 계약 유지. 실제 Motor watchdog·Driver·새 executor 및 Mecanum data contract 변경은 포함하지 않음.
- C++17 clean build, ExecutionMode 존재/부재/잘못된 환경값, Runtime lifecycle·supervision·Supervisor loss·Reset 회귀 PASS.
- 임시 time utility 테스트에서 실제 monotonic 시간, microsecond 변환·실패 -errno 및 freshness 경계값 검증 PASS.
- 기존 통신 및 Timer 포함 Integration 10 lifecycle 회귀 PASS. **v4.3 PASS**.

## v4.2 — Runtime Lifecycle / Error Contract

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

## v4.1 — Mecanum M-0 / Application IPC Data Contract

- `applications/mecanum/data/`에 common/motor/imu/lidar 헤더 추가. Framework 공통 타입과 분리.
- Producer sequence·monotonic timestamp, 3축 VelocityCommand/OdometryData, IMU 단위·valid mask 정의.
- 제품 독립적인 FixedLidarScan container와 타입 특성·양수 capacity static_assert 추가.
- C++17 개별/통합 include, 기본값 및 잘못된 template 인자 거부 검증 PASS.
- 실제 Motor/IMU/LiDAR Element 및 Driver 구현은 포함하지 않음.

## v4.0 — Framework hardening baseline

- v3.0 supervision/recovery 및 Phase 3-8/3-9A/3-9B hardening을 완료한 Framework 기준 상태.
- 이후 v4.1 Mecanum M-0 Application IPC Data Contract 및 v4.2 Runtime Lifecycle / Error Contract의 기준. 세부 이력은 아래 v3.0의 Phase별 항목 유지.

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
