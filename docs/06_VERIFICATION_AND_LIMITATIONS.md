# 06. Verification and Limitations

## 증거 구분

과거 README 통합 작업은 문서·링크·GitHub 상태만 확인했습니다. 각 기능 검증 기록은 해당 시점의 결과이며, 후속 버전 정합성 검증은 별도 절에서 구분합니다.

### Historical temporary verification artifacts

아래 `/tmp/...` script/log 경로는 당시 개발 검증 근거이며 repository의 영구 artifact가 아닙니다.
These paths were used during development verification and are not part of the repository or reproducible release artifact.
해당 파일의 존재는 v5.0 사용/빌드 조건이 아닙니다. 사용자 수동 검증은 별도 출처로 기록합니다.


기존 문서 정리 단계에서는 코드를 변경하거나 기능 테스트를 재실행하지 않았습니다.
기존 v5.0 문서 정리 당시에는 코드/테스트/문서/로그 대조와 `ctest --test-dir build -N` 등록 수 확인을 수행했습니다.
이전 Topic Queue 문서 반영에서는 아래 구현 완료 보고를 기록했으며 빌드·테스트·CTest 조회를 재실행하지 않았습니다.
수치와 PASS는 아래 실행 출처의 범위에 한정됩니다. 하드웨어·hard real-time·무한 부하 보증이 아닙니다.

<a id="v54-local-integration"></a>

## v5.4 로컬 개발 변경 통합 검증 — 2026-10-22

현재 dev working tree의 ExecutionProfile/mlockall, Runtime 주기 복구, Benchmark,
Application Composition/Observer 명시적 재연결 변경을 대상으로 합니다.
**미커밋 개발 변경 검증이며 새 commit/tag/Release 기록이 아닙니다.** 기존 v5.4 버전 정합화 및
아래 v5.3 이하 PASS/FAIL 기록은 당시 사실로 보존합니다. 현재 CMake/FrameworkVersion은 5.4.0입니다.

### 이번 실행과 기존 측정의 구분

- 이번 통합 단계: CMake Debug configure/전체 build, 기존 회귀 및 현재 API 대조.
- 기존 ExecutionProfile 임시 probe는 현재 Core/Bringup에 다시 링크한 후 실행합니다.
  제품 소스/테스트를 수정하거나 실패를 우회하지 않습니다.
- 실행 로그/runner: `/tmp/kcf-v54-integration-final/`. 이는 임시 개발 artifact이며 배포 테스트가 아닙니다.
- 각 실행 항목은 독립 Network/IPC/SHM namespace에서 직렬 실행합니다. Namespace는 시험 자원 격리이며
  물리 LAN/PREEMPT_RT 환경을 대신하지 않습니다. Reset fixture 이름 충돌 방지를 위해 host PID를 유지합니다.
- 아래 1kHz 성능값은 **이전 실제 실행 로그**를 재검토한 것입니다. 이번 통합 단계에서 60초 측정을 재실행하지 않습니다.

### 이번 통합 실행 결과

전체 Debug configure/build PASS. 기존 실행 대상 26개 항목의 **최초 실행은 25 PASS / 1 FAIL**입니다.
이 수치는 CTest 등록 개수가 아니며 Composition runner 내부에서 중복 실행한 회귀는 별도 항목으로 늘리지 않습니다.

| 기능 | 실제 실행 항목 | 이번 결과 |
| --- | --- | --- |
| ExecutionProfile / mlockall | 현재 Core에 재링크한 기존 `/tmp/kcf-profile2-test.cpp` | PASS: 잠금 성공/실패, Setup 전 실패, Setup 실패 후 정리, 내부 env 대체/소비, invalid priority, FIFO EPERM/FD 정리 |
| Runtime lifecycle / 주기 복구 | `kcf_runtime_lifecycle_test` | PASS: 기존 11개 lifecycle case와 초과 Loop 2회. 초과 후 대기 20.344ms / 20.269ms |
| Supervision | `kcf_runtime_supervision_test` + `kcf_supervisor_runtime_test` | PASS: protocol/state/heartbeat/FD |
| Reset | `kcf_reset_test` repeat / failure, 현재 Core에 재링크한 기존 `/tmp/kcf-profile2-reset.cpp` repeat | 3개 항목 PASS: 기본 Reset, 실패 경로, OTHER/단일 affinity/mlock 재적용 |
| Supervisor scope | `kcf_application_scope_test` | PASS: 다른/동일 Application 이름의 상태·Reset·종료 격리 |
| Topic | `kcf_topic_queue_test`, `kcf_topic_recovery_test` | 2개 항목 PASS |
| Parameter / incomplete SHM | `kcf_parameter_recovery_test`, `kcf_incomplete_recovery_test topic/parameter` | 3개 항목 PASS |
| Local Service / IPC 통합 | 기존 Service server/client 1000회, Integration backend/client | 2개 항목 PASS: Topic/Timer, Parameter Get/Set, Service multi-ID, Action Success/Cancel 및 동시 동작 |
| Composition / Observer | 기존 `/tmp/kcf-composition-verify.py`, `/tmp/kcf-observer-r-test.py` | 2개 항목 PASS: Standalone/Managed, ERROR, 이름 충돌/연결 순서/타임아웃, 동일 PID 재연결, SHM 부재, cleanup |
| Network | contract, compatibility, peer_discovery, control_parameter/service/action, topic_data, proxy_recovery, remote_control_client | 9개 항목 PASS |
| Network Remote Topic Client | `kcf_remote_topic_client_test` | **최초 FAIL**, 동일 조건 단독 1회 재실행 **PASS**. 원인 미확정 |

Observer 재연결에서는 정상 재시작 시 원본 Sequence 211→77, 장애 재시작 시 194→77을 새 세대로 수용했습니다.
동일 Observer PID 유지, 새 Timestamp 증가, 중복/역순 0, 최대 Age 3.86245ms / 13.3175ms,
FD/매핑 수 유지, stdin 플래그 복원 및 자식/SHM 회수를 확인했습니다.
재연결 대상 부재 시 이전 Snapshot을 출력하지 않고 DISCONNECTED로 유지하며 사용자 `r` 전 자동 연결하지 않았습니다.

#### 이번 최초 실패와 재실행 기록

`remote_topic_client.log`의 최초 실패는 `remote_topic_client_test.cpp`의 `peer(host)` lambda가
8초 안에 `ONLINE && metadata_ready && 충분한 endpoint 수` 조건을 만족하지 못한 Until assertion입니다.
로그에는 어느 Host/어느 호출 단계인지 식별할 정보가 없으며, 이후 Owner의 recv assertion도 기록됐습니다.
`results.json`: exit -6, 8.48초. 코드·테스트 변경 없이 동일 namespace 조건의 단독 재실행은
`remote_topic_client-recheck.log` / `recheck.json`: exit 0, 4.43초입니다.

재실행 PASS를 최초 26/26 PASS로 소급하지 않습니다. 원인은 미확정이며 기존 간헐적 timeout과
동일 원인인지, 현재 변경에 의한 회귀인지 또는 실행 환경 영향인지 단정하지 않습니다.
따라서 기능별 검증과 문서 정합성 확인은 완료했지만 **완전 무실패 통합 PASS 판정은 보류**합니다.


#### 후속 조사 — Known Intermittent Failure (2026-10-22)

RemoteTopicClient Peer 준비 Timeout은 **Known Intermittent Failure / 원인 미확정**으로 유지합니다.
최초 통합 FAIL 및 단독 재실행 PASS는 위 기록 그대로이며, 후속 실행을 별도로 구분합니다.

| 검증 단계 | 결과 | 관측 |
| --- | --- | --- |
| 진단 추가 전 동일 테스트 5회 | 3 PASS / 2 FAIL | 3번째 8.54초, 5번째 8.60초에 최초와 동일한 Peer 대기 assertion 재현 |
| 테스트 진단 출력 추가 후 Debug 빌드 및 10회 | 빌드 PASS, 10/10 PASS | 각 실행 4.08~4.39초; 이번 반복에서 Timeout 미재현 |

두 단계 모두 해당 테스트만 독립 Network/SHM namespace에서 직렬 실행했습니다.
진단 추가 전 FAIL 로그는 최초 FAIL과 동일하며, 이전 시험의 소켓·SHM 잔재가 원인이라는 근거는 없습니다.
Announce 수신·ONLINE·Metadata 준비 중 실제로 막힌 단계는 당시 로그로 구분하지 못했습니다.

후속 테스트 진단은 단계별 Monotonic 시각, PID·실제 소켓 포트, Host별 Peer 상태,
공개 Stats/오류를 출력하며 조회 API가 없는 항목은 `unavailable`로 표시합니다.
기존 8초 제한, 대기 조건, 시작 순서 및 PASS/FAIL 판정은 유지했습니다.
진단 추가 후에는 Host 10/20 모두 ONLINE 및 `metadata_ready=true`, Metadata revision 2,
Endpoint 수 각각 5/5·4/4를 관측했습니다. 초기 Host 10 대기는 약 95~171ms였습니다.
잔존 테스트 PID는 없었으며 기존 FD/Proxy 정리 검증과 격리 Network/SHM 자원 회수를 확인했습니다.

**10/10 PASS는 원인 해결 또는 간헐적 실패 해소를 의미하지 않습니다.**
진단 출력이 실행 타이밍에 영향을 주었을 가능성을 배제하지 않으며, 원인으로 확정하지도 않습니다.
Timeout이 재현되지 않아 **Timeout 진단 실패 분기는 아직 실행 검증되지 않았습니다.**
추후 실패 로그에서 Host 존재 여부, ONLINE, Metadata 준비·revision·Endpoint 수 및 오류를 확보한 뒤 재조사합니다.
현재 근거로 Timeout 증가나 제품 기능 수정안을 확정하지 않습니다.

v5.4의 요청된 기능 개발 완료와 **전체 회귀 무실패 판정은 별개**입니다.
기능 개발 완료 상태를 유지하되, 이 Known Intermittent Failure가 미해결이므로 전체 회귀 무실패 판정은 계속 보류합니다.
이번 최종 문서 반영에서는 빌드·Benchmark·Network 테스트를 추가 실행하지 않았습니다.

근거 로그: `/tmp/kcf-remote-topic-timeout-investigation/` (5회),
`/tmp/kcf-peer-readiness-diagnostics/` (진단 후 10회 및 `summary.json`).
기존 `/tmp/kcf-v54-integration-final/`의 최초 검증 기록은 보존합니다.
이 경로들은 로컬 임시 검증 artifact이며 저장소에 배포되는 로그가 아닙니다.

### 기존 1kHz VM Benchmark 기록 — 이번 단계 재실행 아님

환경: Ubuntu 24.04.5 LTS / VMware 4 vCPU / kernel 6.8.0-147-generic,
PREEMPT_DYNAMIC, CPU affinity 0–3, RT Priority limit 99, MEMLOCK unlimited.
**PREEMPT_RT 커널 또는 실제 Linux 장치 검증이 아닙니다.** FIFO/10 적용 성공과 PREEMPT_RT 지원은 별개입니다.
3A는 Process::Start/Supervision으로 자식을 관리하고 Runtime 1000Hz를 측정합니다.
`--trace-outliers`는 기본 OFF이며 ON일 때 사전 할당 버퍼에 시각/간격/CPU를 기록하고
종료 후 측정 구간 상위 20개와 전후 5개를 출력합니다. >2/5/10/100ms 집계는 Warm-up을 제외합니다.
Trace ON 자체의 CPU 조회/기록 오버헤드는 별도로 정량화하지 않았습니다.

Release, 매회 60초/최초 5초 제외. 변경 전은 Trace ON A/B/C 각 3회 범위,
변경 후는 주기 복구 수정 후 Trace ON 각 1회입니다. 최대 주기의 변경 전 값은 3회 중 최댓값입니다.

| 조건 | 평균 Hz 변경 전 → 후 | p99 µs 변경 전 → 후 | 최대 µs 변경 전 → 후 | >1500µs 비율 변경 전 → 후 |
| --- | --- | --- | --- | --- |
| A OTHER/0, CPU 0–3 | 975.441–981.964 → 933.874 | 2226.930–2423.909 → 2357.776 | 10045.185 → 7212.194 | 6.890–8.252% → 8.049% |
| B OTHER/0, CPU 0 | 969.620–976.255 → 919.557 | 2504.279–2676.519 → 2697.902 | 8872.466 → 8726.541 | 7.053–8.626% → 9.279% |
| C FIFO/10, CPU 0 | 997.427–998.711 → 991.590 | 1613.476–1623.367 → 1616.784 | 10918.668 → 5369.624 | 2.661–2.731% → 2.538% |

변경 후 >2/5/10ms 횟수는 A 1007/4/0, B 1336/7/0, C 70/1/0입니다.
각 상위 20개 지연 직후 간격 최솟값은 A 1.167ms / B 1.182ms / C 1.180ms로,
확인한 60개 사례에서 이전 15–20µs 즉시 재실행은 없었습니다. 모든 짧은 주기가 제거됐다는 뜻은 아닙니다.
지연 후 한 주기를 대기하는 정책은 평균 Hz를 낮출 수 있습니다. FIFO에서 p99가 더 작더라도
최대 1ms 상한, hard real-time 또는 실행 환경의 지연 원인을 입증하지 않습니다.

초기 Trace OFF B에서 최대 680.441ms를 관측했으나 당시에는 개별 시각/전후 샘플이 없어 원인을 확정하지 못했습니다.
후속 Trace ON 9회(A→B→C, C→A→B, B→C→A)에서는 100ms 초과가 0회여서 해당 지연은 재현되지 않았습니다.
가상화/CPU 경합 등을 원인으로 단정하지 않으며 별도 커널 추적은 수행하지 않았습니다.
출처: `/tmp/kcf-release-results/`, `/tmp/kcf-rt-trace-results/`, `/tmp/kcf-post-recovery-results/`.

3B SHM Topic은 실제 Publisher/Subscriber 두 프로세스와 Runtime 1000Hz를 사용합니다.
Release 기본 OTHER/0, CPU 0–3, 55초 측정에서 주기 복구 변경 전 → 후:

| 지표 | 변경 전 | 변경 후 |
| --- | --- | --- |
| Publish 성공 / 수신 수 | 53,501 / 49,665 | 50,600 / 49,315 |
| 수신 Hz | 903.000 | 896.636 |
| Publish→Callback mean / p99 / max µs | 184.092 / 918.290 / 3065.867 | 190.587 / 992.982 / 3711.794 |
| 원본 Sequence gap | 3,836 | 1,285 |

두 실행 모두 Publish 실패/EAGAIN/중복/역순은 0이며, gap은 latest-value 건너뛰기로 분리했습니다.
Subscriber Loop 관측 지연과 Callback 도착 지연은 다릅니다. 3B CLI의 RT 설정은 제공하지 않으므로
RT Topic 성능으로 해석하지 않습니다. 이전 Debug 결과 역시 다른 시점/조건의 관측값이므로
Release 최적화나 주기 수정의 효과만으로 차이를 단정하지 않습니다.

### 남은 제한 및 미검증

- PREEMPT_RT **실장 검증 미수행**. 접근 가능한 환경은 기존 VM뿐이며 실제 장치의 커널/권한/빌드 가능 여부는 미확인.
- 물리 LAN Discovery 지연은 **미해결 사항으로 유지**합니다. 이번 Local/격리 Network 회귀 PASS로
  물리 LAN 지연 원인·재발견 시간·장애 후 자동 복구 안정성을 해결했다고 판단하지 않습니다.
- 기존 Network Service/Topic/Action의 간헐적 timeout 및 재실행 PASS 이력은 아래/Network 문서에 보존합니다.
- RT 권한/MEMLOCK은 OS 조건이며 자동 권한 상승·커널 변경을 제공하지 않습니다.
  테스트에서 실패 조건을 만들기 위한 한도/권한 하향은 해당 테스트 자식에만 적용됩니다.
- Composition은 일반 실행 예제입니다. External Observer는 자동 재연결하지 않으며 stdin EOF 시
  기존 관측만 계속합니다. SIGKILL 이후 owner SHM 회수에는 기존 소유권/세대 계약과 명시적 재실행이 필요합니다.
- `/tmp`의 Profile probe·Composition runner 및 측정 로그는 영구 CI/배포 artifact가 아닙니다.
  실제 실행 항목과 내부 assertion 수, CTest 등록 수를 혼동하지 않습니다.

## v5.3 Network Diagnostics 검증

F-ND1~F-ND5A 적용 dev working tree와 별도 Tool 저장소의 기존 실행 로그를 대조한 기록입니다.
**F-ND5B에서는 build/test를 재실행하지 않았습니다.** 아래 PASS는 각 실행 출처에 한정되며,
새 공개 commit/Release 또는 물리 LAN/데스크톱 수동 검증으로 해석하지 않습니다.

| 실행 / 출처 | 결과 | 범위 |
| --- | --- | --- |
| F-ND5A `/tmp/kcf-fnd5a/configure.log`, `build.log` | PASS | 전체 Debug C++17 build, Framework/CMake 5.3.0 |
| F-ND5A `/tmp/kcf-fnd5a/results.json` | 8/8 PASS | contract, compatibility, Discovery, Parameter, Service, Action, Topic, proxy recovery |
| F-ND5A `control_parameter.log` | PASS | opt-in/disabled, public client, replay 회수/상한, timeout/partial IO/identity gates |
| F-ND5A `topic_data.log` | Diagnostics Loopback E2E PASS | 두 실제 DataPlane, proxy Subscriber, public client, route/counter 상관관계, Control 동시 부하 |
| F-ND5A `/tmp/kcf-fnd5a/virtual/results.json` | Framework Virtual LAN PASS | peer/NIC 장애, netem, proxy recovery, cleanup |
| KT-ND2 `/tmp/kt-nd2-virtual-final/results.json` | Tool 독립 Virtual LAN Diagnostics E2E PASS | 별도 kcf_tools 저장소의 독립 Observer + Qt offscreen; Framework 자체 시험과 구분 |
| KT-ND3 `/tmp/kt-nd3/virtual/results.json` | 후속 Tool Virtual LAN PASS | 별도 실행이며 KT-ND2의 측정값과 혼합하지 않음 |

F-ND5A 최초 sandbox 실행은 netlink `Operation not permitted`로 테스트 시작 전에 막혔습니다.
`/tmp/kcf-fnd5a/sandbox-preflight.log`와 `sandbox-preflight-results.json`을 보존했고,
격리 namespace 실행이 허용된 환경에서 위 8/8을 확인했습니다. 문서 작업에서 이 검사를 재실행하지 않았습니다.
F-ND5A의 임시 version-check는 CMake와 FrameworkVersion 일치, 기본 Diagnostics OFF,
5.2↔5.3 NETWORK_PROTOCOL 호환 및 EXACT mismatch를 확인했습니다. 기존 wire/storage 상수는 유지됐습니다.

### 측정값의 출처와 의미

아래는 **KT-ND2 별도 Tool Virtual LAN**의 `real_backend_rates_graph` 관측값을 반올림한 것입니다.
출처는 `/tmp/kt-nd2-virtual-final/results.json`이며 F-ND5A Loopback 수치가 아닙니다.

| 관측 | 값 |
| --- | --- |
| Source Local Publisher | 200.22 Hz |
| Network Send | 46.67 Hz |
| Network Receive | 47.07 Hz |
| Gateway Proxy Publish | 47.07 Hz |
| Local samples skipped | 152.58/s |

Source Publisher rate는 fixture의 실제 발행 횟수/관측 시간으로 측정했습니다.
다른 rate는 Gateway counter의 차이와 Tool 관측 시간을 이용합니다. 관측 창이 다르므로 Send/Receive 수치가
정확히 같을 필요는 없습니다. Framework는 Hz 자체가 아닌 counter를 제공합니다.
약 200 Hz Local 발행과 약 50 Hz Network sampling은 서로 다른 경로이며 skipped는 Network packet loss와 다릅니다.
Proxy Publish는 Local SHM 쓰기율이지 Application Subscriber의 처리율이 아닙니다.
이 값들은 해당 실행의 관측치이며 성능 보장·고정 테스트 기대값이 아닙니다.

### 간헐적 timeout 이력 보존

- F-ND4B 최초 `/tmp/kcf-fnd4b-regression.log`: Topic `Conflict()`의 `Until` assertion 실패.
  후속 Topic 단독 실행은 PASS (`/tmp/kcf-fnd4b-final-topic.log`). 최초 전체 실행을 PASS로 바꾸지 않습니다.
- F-ND4B `/tmp/kcf-fnd4b-regression-recheck.log`: Action `Integration()`의 `Until` assertion 실패.
  `/tmp/kcf-fnd4b-action-recheck.log` 단독 재실행 PASS. 원인은 확정하지 않았습니다.
- KT-ND3 `/tmp/kt-nd3/review.md`: Framework Network 최초 7/8 PASS, Topic `Integration()` timeout.
  `framework-topic_data.log`는 어떤 Integration lambda인지 확정할 정보를 제공하지 않습니다.
  `framework-topic_data-retry.log` 단독 재실행 PASS. CPU 부하·Discovery·namespace 충돌 등을 원인으로 단정하지 않습니다.
- F-ND5A의 별도 8/8 PASS는 위 실패 이력을 지우거나 간헐적 문제 해결을 입증하지 않습니다.
  v5.2 proxy recovery 당시 최초 대기 실패·재실행 이력도 기존 Network 문서에 그대로 남깁니다.

### 범위 및 남은 제한

- 실제 물리 LAN, 이기종 NIC/CPU, 데스크톱 GUI 수동 검증은 미실행입니다. Virtual LAN과 Qt offscreen으로 대체 판정하지 않습니다.
- Tool 수동 Refresh / Graph freshness 10초는 Tool 정책입니다. Framework Query deadline이나 route TTL이 아닙니다.
- Snapshot 16 KiB 잘림은 성공 응답일 수 있으며 전체 route inventory/모든 counter 이력을 보장하지 않습니다.
- read-only route 관측 API를 추가했지만 인증·암호화·권한 기능, 자동 retry, Reliable Topic, Action re-attach는 추가하지 않았습니다.
- Tool v0.12의 외부 Diagnostics 미지원 기록은 당시 사실입니다. v5.3 공개 API와 별도 Tool 개발 연동으로 관측 경로가 마련됐으며,
  이 문서가 Tool v0.13의 공개·배포를 선언하지는 않습니다. Remote Set/Call/Goal/Cancel UI도 추가하지 않았습니다.
- `/tmp/` 로그는 임시 개발 artifact입니다. 이전 SHA 기반 검증 및 v5.2·Tool v0.12 기록을 새 실행 결과로 소급하지 않습니다.

## v5.2 공개 후 버전 정합성 검증

이 절은 당시 `0a3f2a6` 기반 checkout에 미커밋 보정을 적용해 수행한 historical 검증 기록입니다.
해당 version/document alignment 변경은 이후 KCF v5.2의 5.2.0 version-alignment commit
`d41fc2fc71d3b60b0475035ec56ac32b92ffb849`로 반영되었습니다(Framework v5.2 / 5.2.0).
아래의 “이번”, “미커밋”, “commit/push 미수행”은 당시 작업을 가리킵니다.
이번 공개 상태 문서 마감에서는 build/test를 재실행하지 않았으며,
아래 PASS를 `d41fc2f` clean commit에서 실행한 결과로 소급하지 않습니다.

기준: GitHub dev 및 로컬 HEAD `0a3f2a630cec209793f9ac6e99107752e9e01283`에
이번 미커밋 버전/주석/문서 보정을 적용한 checkout입니다. **공개 commit 자체의 clean 검증과는 다릅니다.**
`git ls-remote`로 Framework dev의 위 SHA와 kcf_tools main의
`f4fb4471fc0b03b24663eb20eede8d8491fd87f6`을 확인했습니다. Tag/Release 여부는 확인하지 않았습니다.

이번에 실제 실행한 결과:

- 새 `/tmp/kcf-v52-version-check`에서 C++17 Debug 전체 configure/build: PASS.
- 기존 `kcf_network_compatibility_test`, `kcf_network_contract_test`: PASS. 테스트 내용 변경 없음.
- 기본 FrameworkVersion=5.2.0, NetworkProtocolVersion=1.0, standalone DISCOVERY-only 확인: PASS.
  별도 임시 C++ 확인에서 5.2.0 ↔ 5.1.0은 NETWORK_PROTOCOL 정책에서 compatible,
  EXACT_FRAMEWORK_VERSION에서는 mismatch, 5.2.0 ↔ 5.2.0 exact는 compatible임을 확인했습니다.
- 기존 Network 회귀 8개 실행 항목 중 최초 7개 PASS, Discovery codec assertion 1개 FAIL.
  원인은 기본 Framework minor의 wire byte 기대값 `bytes[83]==1`이었습니다.
  **이 기대값만 `2`로 수정**하고 Discovery 전체 테스트를 재빌드·재실행하여 PASS했습니다.
  Discovery envelope/offset/길이 및 malformed/이전 version 거부 의미는 변경하지 않았습니다.
- 최종 각 항목 PASS: contract, compatibility, Discovery, Remote Parameter, Remote Service,
  Remote Action, Remote Topic, proxy recovery. 사용자 IPC와 분리된 PID/SHM namespace에서 실행했습니다.
- `git diff --check`와 수정 문서 로컬 파일 링크 검사: PASS.

실행 명령:

```sh
cmake -S . -B /tmp/kcf-v52-version-check -DCMAKE_BUILD_TYPE=Debug
cmake --build /tmp/kcf-v52-version-check -j4
```

회귀는 해당 build의 `examples/network/kcf_*_test` executable을 순차 실행했습니다.
로그: `/tmp/kcf-v52-configure.log`, `/tmp/kcf-v52-build.log`, `/tmp/kcf-v52-regression/`.
`results.json`은 최초 실행(Discovery 실패 포함), `discovery-recheck.log`는 수정 후 PASS 기록입니다.
이 경로는 개발 검증용 임시 artifact이며 영구 배포물은 아닙니다.

Runtime/Network 동작 변경은 Framework 광고 기본 버전 변경뿐입니다. NetworkProtocolVersion 1.0,
Discovery v2, Control TCP v1, Topic wire v1, Topic Format 4, Parameter Format 3,
Local Service protocol 2는 유지했습니다. Compatibility 정책 구현과 나머지 테스트 코드는 그대로입니다.
Virtual LAN 전체 시험, 물리 LAN, sanitizer, 전체 Local 회귀 및 kcf_tools 테스트는 이번에 재실행하지 않았습니다.
아래 기존 기록과 Network/Virtual LAN 문서의 과거 timing 실패·재실행 결과를 새 PASS로 대체하지 않습니다.
이번 보정의 commit/push는 수행하지 않았습니다.

## 기존 v5.0 Automated / Regression Verification

[AUTOMATED VERIFIED] Framework sequential runner 22/22 PASS.
Introspection R1~R5, Runtime/Supervisor/recovery/timer/integration을 포함합니다.

### Framework: CTest 0, sequential runner 22

기존 v5.0 확인 당시 Framework top-level CMake의 `ctest -N` 결과는 **Total Tests: 0**이었습니다.
최근 Action app 작업의 `/tmp/action-app-regressions.py`가 **20개 case + legacy Service + Integration = 22개 실행 항목**을
순차 실행했고 최종 `FAILED: []`였습니다. 각 항목에는 여러 내부 assertion/scenario가 포함됩니다.
22개 CTest test가 있다는 뜻이 아닙니다.

| # | runner label | 실제 실행 대상 (`build/` 기준, Python은 source 기준) |
| --- | --- | --- |
| 1 | r41 | examples/introspection/kcf_shm_incarnation_test |
| 2 | r5 | examples/introspection/kcf_service_introspection_test |
| 3 | r4 | examples/introspection/kcf_dynamic_access_test |
| 4 | r3 | examples/introspection/kcf_type_descriptor_test |
| 5 | r2b | examples/introspection/kcf_supervisor_discovery_test |
| 6 | named_cli | 위 executable --cli build/bringup/kcf_bringup |
| 7 | r2a | examples/introspection/kcf_endpoint_registry_test |
| 8 | discovery | examples/introspection/kcf_runtime_discovery_test |
| 9 | lifecycle | examples/supervisor/kcf_runtime_lifecycle_test |
| 10 | runtime_health | python3 examples/supervisor/runtime_health_checks.py build |
| 11 | supervisor_loss | python3 examples/supervisor/supervisor_loss_checks.py build |
| 12 | reset | python3 examples/supervisor/reset_checks.py build |
| 13 | system_status | examples/supervisor/kcf_system_status_recovery_test |
| 14 | topic_recovery | examples/recovery/kcf_topic_recovery_test |
| 15 | parameter_recovery | examples/recovery/kcf_parameter_recovery_test |
| 16 | incomplete_topic | examples/recovery/kcf_incomplete_recovery_test topic |
| 17 | incomplete_parameter | examples/recovery/kcf_incomplete_recovery_test parameter |
| 18 | runtime_protocol | examples/supervisor/kcf_runtime_supervision_test + kcf_supervisor_runtime_test executable path |
| 19 | application_scope | examples/supervisor/kcf_application_scope_test |
| 20 | timer | examples/timer/kcf_timer_test |
| 21 | legacy_service | kcf_service_server port + kcf_service_client port 1000, 정상 server 종료 |
| 22 | integration | kcf_integration_backend + kcf_integration_client, 정상 backend 종료 |

기존 v5.0 기록의 결과: **22/22 PASS**. 로그 디렉터리: `/tmp/action-app-regression/`.
마지막 두 항목은 concurrent server/client pair를 runner가 관리합니다. `python3 /tmp/action-app-regressions.py`는
당시 개발 환경에서 사용한 실행 명령이며 runner는 repository에 포함되지 않은 임시 artifact입니다.
배포된 checkout에 그 파일이 존재한다고 보장하지 않습니다. 개별 source/target은
[supervisor](../examples/supervisor/), [introspection](../examples/introspection/), [recovery](../examples/recovery/),
[integration](../examples/integration/)에서 확인할 수 있습니다.

### Application-level Automated Verification

| 예제 | 확인된 결과 | 실제 근거 artifact |
| --- | --- | --- |
| PubSub | standalone/supervised, matching samples, 양 endpoint, Echo 5 fields, 정상 cleanup PASS | `/tmp/pubsub-ui-run.log`, `/tmp/pubsub-supervised.log` |
| Parameter | initial Get, Apply/re-read/owner readback/watcher, Revert, invalid input, stale, restart PASS | `/tmp/parameter-ui-run.log`, `/tmp/parameter-standalone.log`, `/tmp/parameter-supervised.log` |
| Service | form, signed/zero, validation, duplicate guard, timeout, old identity handler 미호출, restart PASS | `/tmp/service-ui-run.log`, `/tmp/service-standalone.log`, `/tmp/service-supervised.log` |
| Timer | period, monotonic ticks, SIGTERM/SIGINT, post-Shutdown no ticks, restart PASS | `/tmp/timer-app-runtime.log`, `/tmp/timer-app-regression/timer.log` |
| Action | Success/Cancel, invalid goal, bounded server-exit failure, Supervisor ERROR, restart, legacy Action PASS | `/tmp/action-app-tests.log`의 완료 항목 + `/tmp/action-app-tail.log`, `/tmp/action-supervised.log`, `/tmp/action-crash.log` |

Action의 최초 임시 테스트에는 global discovery count 가정, startup barrier 이전 kill,
Supervisor 오류 종료 코드를 0으로 기대한 test 오류가 있었습니다. 마지막 tail 검증은 RUNNING 이후 kill과
오류 종료를 기대해 통과했습니다. 앞선 traceback을 숨기거나 전체 로그를 무조건 성공으로 간주하지 않습니다.
Timer discovery 임시 테스트도 해당 Runtime 범위로 보정했습니다. 제품 코드 수정으로 우회하지 않았습니다.
PubSub 작업 당시 scope test 최초 1회 timeout 후 재실행 PASS가 있었으며 원인은 확정하지 않았습니다.
최근 Action 단계의 22개 회귀는 전체 통과했습니다.

Timer 숫자는 측정 구간별 값입니다. Timer app 로그 간격은 500.0/500.1/489.9 ms이고,
최근 Action 단계의 low-level Timer 회귀 2 Hz callback 평균은 **500.025 ms**입니다.
다른 단계의 499.989 ms 측정과 혼합하지 않습니다.

### Tool: 기존 문서에서 확인한 과거 결과

KT-8 (`Nyamkani/kcf_tools/docs/KT8_GENERIC_CROSS_APPLICATION_VALIDATION.md`) 및
release notes (`Nyamkani/kcf_tools/docs/V0_1_RELEASE_NOTES.md`), 기록일 2026-09-15:
KT-8 **1/1**, Tool regression **10/10**, KCF-less GUI **3/3**, KCF-less backend **1/1 PASS**.
이는 이번 문서 작업의 재실행 결과가 아닙니다. 당시 Framework는 runner 19 + 추가 2 항목이며
최근 22개 runner와 구성/시점이 다릅니다.

최근 app 예제의 Tool 검증은 기존 UI/backend source를 수정 없이 링크한 임시 Qt offscreen harness의 자동 입력·버튼 조작입니다.
기존 v0.1 binary 실행도 별도 확인했지만, 기존 binary에 대한 모든 클릭을 OS automation으로 수행했다는 뜻은 아닙니다.

<a id="topic-queue-verification"></a>

## Topic Queue — v5.1 확정 이전 구현 검증 보고

[REPORTED RESULT] 출처는 v5.1 확정 이전의 **Topic Queue 구현 완료 보고**입니다.
아래 PASS는 해당 구현 작업에서 보고된 결과이며, **당시 문서 반영 중 빌드나 테스트를 재실행하지 않았습니다**.
기존 v5.0의 22개 회귀·GUI 수동 검증 및 아래 v5.1 최종 실행 결과와 구분합니다. 당시에는 버전 확정 전의 개발 변경으로 기록했습니다.

| 검증 | 보고된 결과 | 범위 |
| --- | --- | --- |
| 전체 빌드 | PASS | C++17 전체 build |
| 신규 Queue 필수 검증 | 10개 영역 PASS | `examples/recovery/kcf_topic_queue_test` 및 관련 회귀; 아래 영역 구분 |
| 기존 회귀 | 19개 항목 PASS | `examples/introspection/r41_regressions.py`의 17개 case + legacy Service + Integration |
| 기존 kcf_tools Backend/Topic Echo | PASS | Tool 소스 수정 없이 새 Core와 연결한 `kcf_tool_real_backend` 테스트; GUI 수동 재검증을 의미하지 않음 |

Queue 필수 검증 영역은 다음과 같습니다. 10개 독립 CTest 등록 수라는 뜻은 아닙니다.

1. Depth=1 Snapshot 회귀.
2. Depth=N 메시지 보관 순서.
3. 서로 다른 속도의 Subscriber와 독립 Cursor.
4. Overflow 이후 정확한 누락 개수.
5. Reader 복사 중 Writer 덮어쓰기 방지.
6. 실패한 Publish의 Sequence 및 Queue 상태 유지.
7. Reader 비정상 종료와 잔존 pin 한계.
8. Producer 종료·재시작, SHM 재생성 및 명시적 재연결.
9. Depth와 무관한 DynamicTopicReader 최신값 조회.
10. `valid=false` Payload의 변경 없는 전달.

19개 회귀는 위 기존 v5.0 표의 **1~17번, 21~22번**에 해당합니다.
Runtime Health, Supervisor Loss, Reset, Topic/Parameter 복구 및 incomplete storage 검사를 포함하며,
기존 22개 전체를 이번 Queue 변경으로 다시 검증했다고 표현하지 않습니다.
구현 검증 중 구형 SHM Layout/Format을 직접 사용하던 테스트가 실패하여 테스트 매핑·Format 지정만 보정한 뒤
관련 검증을 통과한 것으로 보고됐습니다. 검증 조건이나 production 복구 기능을 제거한 결과가 아닙니다.

보고된 실행 근거: repository의 [Queue 테스트](../examples/recovery/src/topic_queue_test.cpp),
[기존 회귀 runner](../examples/introspection/r41_regressions.py), 개발 환경의 `build/r41-validation/`,
`/tmp/queue-final-build.log`, `/tmp/queue-reset-build.log`, `/tmp/kcf-topic-queue-tool-validation/`.
로그·빌드 경로는 영구 artifact나 현재 존재 보장이 아닙니다.

<a id="v51-final-verification"></a>

## v5.1 최종 실행 검증

[AUTOMATED VERIFIED — FINAL RUN] **2026-09-19**, `dev`의 v5.1 작업 트리에서 실제로 실행했습니다.
CMake project version은 5.1.0이며 새로운 `build/v5.1` 디렉터리에서 C++17 Debug 전체 빌드를 수행했습니다.
이 절은 위의 기존 v5.0 기록 및 Queue 구현 완료 보고를 복사한 결과가 아닙니다.

| 검증 | 이번 실행 결과 | 대상 |
| --- | --- | --- |
| Configure / 전체 빌드 | PASS | C++17 Debug, 기존 example 포함 |
| Topic Queue | 필수 10개 영역 PASS | `kcf_topic_queue_test`; 영역 정의는 위 Queue 보고 목록과 동일 |
| 기존 회귀 runner | 19/19 PASS | `r41_regressions.py`: 17개 case + legacy Service + Integration, `FAILED: []` |
| 추가 Framework 회귀 | 3/3 PASS | Runtime protocol, Application scope, Timer |
| Framework 회귀 합계 | 22/22 PASS | 이번 19개 runner와 추가 3개 실행의 합계; CTest 등록 개수를 의미하지 않음 |
| 기존 kcf_tools Backend/Topic Echo | 1/1 PASS | Tool 소스 수정 없이 새 Core로 별도 빌드한 `kcf_tool_real_backend` |

Framework 실행 명령:

```sh
cmake -S . -B build/v5.1 -DCMAKE_BUILD_TYPE=Debug
cmake --build build/v5.1 -j4
./build/v5.1/examples/recovery/kcf_topic_queue_test
python3 examples/introspection/r41_regressions.py build/v5.1
./build/v5.1/examples/supervisor/kcf_runtime_supervision_test ./build/v5.1/examples/supervisor/kcf_supervisor_runtime_test
./build/v5.1/examples/supervisor/kcf_application_scope_test
./build/v5.1/examples/timer/kcf_timer_test
```

Tool은 기존 외부 `kcf_tools` 저장소를 사용하고 `KCF_SOURCE_DIR`를 현재 KCF 저장소로 지정했습니다.
`KCF_TOOL_BUILD_GUI=OFF`, `KCF_TOOL_BUILD_TESTS=ON`, Debug로 `/tmp/kcf-v51-tool-validation`에
`kcf_tool_real_backend`와 `kcf_tool_real_fixture`를 빌드한 뒤 다음을 실행했습니다.

```sh
ctest --test-dir /tmp/kcf-v51-tool-validation -R '^kcf_tool_real_backend$' --output-on-failure
```

이번 실행 로그: `/tmp/kcf-v51-build.log`, `/tmp/kcf-v51-queue.log`, `/tmp/kcf-v51-regressions.log`,
`build/v5.1/r41-validation/` (추가 3개 로그 포함), `/tmp/kcf-v51-tool-build.log`, `/tmp/kcf-v51-tool-test.log`.
이 경로는 개발 환경의 임시 산출물이며 repository에 포함하거나 영구 보존을 보장하지 않습니다.
이 테스트는 GUI 수동 재검증, 실제 장치 검증, hard real-time 또는 dead-reader pin 자동 회수를 보장하지 않습니다.
당시 검증 이후 v5.1은 GitHub dev의 [82fa3b2](https://github.com/Nyamkani/kss_control_framework/commit/82fa3b274f51d847602b2a5e4936778ebd631257)에 반영되었습니다.
현재 GitHub 반영·tag/Release 확인 상태는 [v5.1 상태](V5_1_STATUS.md)에 기록합니다.

## Manual GUI Verification

[MANUALLY VERIFIED] 아래는 기존 v5.0 보정 요청에서 사용자가 제공한 **직접 GUI/process 검증 이력**이다.
자동 regression fixture 결과나 이번 문서 수정 중 재실행한 결과로 분류하지 않는다.

### Pub/Sub

- Supervised `pubsub_example` RUNNING 및 publisher/subscriber 2개 Element 확인.
- Topic Echo의 5개 field와 실제 증가 값 확인.
- Subscriber SIGKILL 후 Supervisor RUNNING → ERROR, healthy publisher 계속 실행 확인.
- Tool Refresh 전 기존 snapshot 유지, Refresh 후 failed subscriber 제거 및 publisher만 남는 topology 확인.
- Bringup 전체 종료·재실행 후 새 Supervisor/child PID/start_ticks generation 확인.
- Standalone publisher/subscriber discovery 확인.

Result: **Manual GUI / process failure / restart PASS**.
이 결합 scenario는 manual integration verification이며 automated regression fixture가 아니다.

### Parameter

`parameter_example` RUNNING, `/example/config` OWNER, field descriptor 표시,
Edit/Apply, 변경값 재조회, Owner readback, Revert를 직접 확인했다.
Result: **Manual GUI PASS**.

### Service

`service_example` RUNNING과 `/example/add` 표시를 확인하고 request a=10, b=25를 입력했다.
Response result=35, accepted=true를 확인했다. Result: **Manual GUI PASS**.

## Manual Console Verification

[MANUALLY VERIFIED] 사용자 직접 console/runtime 확인 이력이다.

### Timer

Standalone/supervised 500 ms periodic output, tick 증가, shutdown 후 tick 중단 확인.
Result: **Manual console PASS**. 정밀한 hard real-time 보장을 의미하지 않는다.

### Action

- Success: target=10, IDLE → ACCEPTED → RUNNING → SUCCEEDED,
  feedback sequence/count 1~10, progress 0.1 → 1.0, final=10, result_code=0.
- Cancel: target=50, feedback 진행 후 약 500 ms에 Cancel, CANCELED,
  result_code=-ECANCELED 및 cancel 이후 feedback 중단.
- Client는 두 scenario 완료 후 idle/RUNNING 유지.

Result: **Manual console PASS**. 취소 final count는 타이밍에 따라 달라진다.
Hardware safety / hard real-time / 실제 Device I/O는 이번 v5.0 Framework 검증 범위가 아니다.

## Historical v1.0–v4.3 — 통합 전 README의 검증 요약

[HISTORICAL RECORD] 아래는 [통합 전 README 원문](https://github.com/Nyamkani/kss_control_framework/blob/82fa3b274f51d847602b2a5e4936778ebd631257/Readme.md)의 개발 당시 보고를 요약한 것입니다.
**이번 문서 정리에서 재실행하지 않았으며**, v5.1 최종 실행 결과와 구분합니다.
상세 단계·변경은 [Changelog](../Changelog.md)에 보존합니다.

| 과거 범위 | 보존한 검증 기록 |
| --- | --- |
| v1.0 | clean build, Dummy 2 Hz, signal 종료, Bringup child 생성·회수 및 zombie 부재 |
| v2.0 통신 | Topic snapshot 무결성·느린 수신·초기화 경합; Timer 2/100 Hz·Stop·overrun·Topic 300회; Service 1,000회·timeout/retry·중복·port 충돌 |
| v2.0 Parameter / Action | Parameter Set 4,000회·lifecycle 100회; Action FSM·중복 Goal·Feedback 2,000회·lifecycle 25회·FD/SHM 정리 |
| v3.0 Phase 3-7 | 네 fault·최초 오류·SAFE, Reset 10회·운전 gate·강제 정리, 20 Element 관찰, 통신 및 Integration 10 lifecycle |
| Phase 3-8 | Supervisor SIGKILL/SIGSTOP, supervised 종료·reap, Standalone 영향 없음 |
| Phase 3-9A | 당시 SystemStatus의 동일 객체에서 reader crash 20회 복구, Reset 중 inode·최초 오류 보존 |
| Phase 3-9B | Topic/Parameter 각각 404회 Open/Create 경합, live owner/unknown header 보호, initializer crash 후 Reset |
| v4.2 | Setup/Loop 성공·오류·예외, 정확히 1회 Shutdown, 오류 우선순위·실패 cycle heartbeat·partial cleanup, Runtime/Reset/Loss/통신 회귀 |
| v4.3 | ExecutionMode 및 기존 회귀; Application monotonic utility의 실제 clock·변환·오류·freshness 경계값 임시 테스트 |

과거 global SystemStatus와 Topic latest-only 설명은 당시 구조입니다. 현재 SystemStatus는 Application-scoped이고
Topic v5.1은 Depth Queue입니다. v4.1 Mecanum M-0 및 v4.3 Application time utility는 과거 Application 개발 이력이며
현재 Framework repository에 Mecanum 소스가 포함된다는 의미가 아닙니다. 1 kHz Loop 관찰 역시 hard real-time 보장이 아닙니다.

## Known Limitations

| ID | Area | Limitation | Impact | Current Mitigation | Future Option | Status |
| --- | --- | --- | --- | --- | --- | --- |
| L-001 | SharedChannel | dead-reader pin 잔존 | slot 제약/-EAGAIN | bounded 반환 처리, controlled owner recovery | reader liveness 회수 | Open |
| L-002 | SharedParameter | synchronous locking | reader/worker 정지가 접근을 block 가능 | 짧은 작업·dead-owner robust recovery | bounded isolation | Open |
| L-003 | Service | manual UDP port | bind 충돌 | Application이 서로 다른 port 지정 | 명시적 관리 도구 | Retained |
| L-004 | Service identity | 검증과 송신 non-atomic, wire registration_id 없음 | 마지막 검사 직후 재등록 race | stale 사전 검사/명시적 재선택 | protocol generation token | Open |
| L-005 | Service timeout | timeout != 미실행 | 무분별한 재호출 위험 | 오류 표기·Tool 추가 retry 없음 | Application idempotency 설계 | Contract |
| L-006 | Dynamic access | stable descriptor 필요 | legacy/unknown type generic 접근 제한 | explicit TypeDescriptorTraits | schema 확장 | Contract |
| L-007 | Timer | introspection endpoint 없음 | Tool Timer 표시 불가 | Runtime만 발견 | 별도 요구 검토 | Intentional |
| L-008 | Action | Tool 전용 UI/model 없음 | Goal/Result GUI 조작 불가 | app client 사용 | Action model | Deferred |
| L-009 | Tool discovery | manual Refresh | topology/state 자동 갱신 아님 | Refresh/reselect | 관찰 lifecycle | Retained |
| L-010 | Topic Echo | polling, Dynamic Monitor 없음 | sample 누락·poll latency | persistent 100 ms reader | callback monitor | Retained |
| L-011 | Introspection | fixed capacities | metadata 전체 노출 못 할 수 있음 | 규모/등록 결과 관찰 | capacity 정책 | Open |
| L-012 | ABI | 호환 Framework/native layout 필요 | 다른 build/ABI 간 통신 보장 없음 | 동일 호환 revision rebuild/restart | portable serialization | Contract |
| L-013 | SHM | Topic format 4 / Parameter format 3 호환 검사 | 이전 format 자동 migration 없음 | owner lifecycle과 호환 build 관리 | migration 정책 | Contract |
| L-014 | Service | protocol 2 호환 검사 | 이전 protocol과 호환 보장 없음 | client/server 함께 rebuild | version negotiation | Contract |
| L-015 | Namespace | global logical SHM ownership | 같은 Topic의 복수 owner 충돌 | explicit prefix/port별 이름 | namespace 정책 | Open |
| L-016 | Action | single active goal 중심 | queue/concurrent/preemption 미지원 | 순차 scenario | 정책 확장 | Retained |
| L-017 | SystemStatus | 외부 observer에 Application identity 필요 | 이름만으로 자동 연결 안 됨 | OpenForApplication(pid,start_ticks) | explicit resolver | Contract |

L-011 현재 값: endpoints/runtime **128**, types/runtime **64**, services/runtime **64**,
Supervisor metadata elements **64**, descriptor fields **128**. 이는 introspection 용량이며
그대로 process 실행 수의 hard limit으로 일반화하지 않습니다.
근거: [introspection detail](../kcf/include/kcf/introspection/detail/),
[SupervisorInfo](../kcf/include/kcf/introspection/supervisor_info.hpp), [TypeDescriptor](../kcf/include/kcf/introspection/type_descriptor.hpp).
기능별 구현 근거는 [03](03_IPC_AND_FEATURES.md), identity race는 [04](04_INTROSPECTION_AND_TOOL.md) 참조.

추가 한계: Linux kernel uninterruptible sleep(D state)은 SIGKILL 이후에도 userspace에서 bounded 종료를 보장하지 못합니다.
Runtime Parameter의 디스크 영속 저장은 제공하지 않습니다.
그 외 blocked Setup/Loop의 강제 cleanup 불가, registry 조회의 synchronous 접근,
죽은 process의 stale metadata가 남을 수 있음, 인증/remote security boundary 미제공.
/tmp artifact는 영구 보존을 보장하지 않으므로 장기 release 증거로 필요하면 별도 보존 절차가 필요합니다.

Topic Queue 추가 계약·한계: Topic당 단일 Publisher/쓰기 thread, 논리 Entry당 3개 Payload Slot의 메모리 비용,
KEEP_LAST Overflow에 따른 유실 가능성을 유지합니다. Dead-reader pin은 자동 회수하지 않으며 안전한 Slot 부족은
`-EAGAIN`입니다. Typed Subscriber는 기존 mapping에서 새 SHM으로 자동 이동하지 않으므로 명시적 Close/Create
(SharedChannel은 Close/Open)가 필요합니다. DynamicTopicReader는 재생성을 `-ESTALE`로 알립니다.
Transport Sequence 최대값에서는 Publish가 `-EOVERFLOW`이며, Payload Timestamp/Validity의 의미와 판단은 Application 책임입니다.
이전 Topic Format 3과 Format 4는 바이너리 호환되지 않아 참여 바이너리 재빌드가 필요하고 자동 삭제·변환은 하지 않습니다.
