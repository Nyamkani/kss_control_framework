# KCF Core / KCF Tools 통합 검증 및 설계 검토 보고서

- **기록일:** 2026-11-26 (KST)
- **대상:** KCF Core v5.6.1 개발 계열 / KCF Tools v0.15 개발 계열
- **상태:** VM 통합 검증 완료, 물리 장치 재검증 및 Issue 정리 대기
- **성격:** 개발·검증 기록. 정식 Release/Tag 또는 `dev`/`main` 병합 완료 선언이 아님

## 1. 개요 및 결정

이번 작업은 Ubuntu 24.04 개발 VM에서 Core·Tools를 결합해 기능·가상 네트워크·프로세스 수명주기를 확인하고, Gateway Topic 100 Hz 성능 한계와 Timestamp 및 수신 복구 방향을 검토한 것이다.

**최종 결정:**

1. **Core와 Tools의 기존 기능 통합 검증은 PASS.** 다만 각 시험이 실제로 다룬 범위만 인정한다.
2. **Gateway 100 Hz 송신률 편차는 Known Limitation.** 같은 SHM 샘플의 재관측, 독립 스케줄 위상 및 VM 프로세스 지연의 영향이 있으며, 모든 환경에서 항상 재현되거나 100 Hz를 보장하지 못한다는 뜻으로 단정하지 않는다.
3. **TT1 Timestamp Tolerance는 Topic별 opt-in, 기본 OFF로 유지.** 기능 검증은 통과했으나 OFF 대비 일관된 성능 우위는 확인되지 않았다.
4. **RR1 UDP 손실 복구는 설계 단계에서 중단.** 수신 버퍼만으로 실제 UDP 유실을 복구할 수 없으므로 송신 캐시·재전송 요청 등 별도 프로토콜 설계가 필요하다.
5. **공통 SHM Publish Timestamp는 후속 IPC 포맷 개정 과제.** Timestamp 표준화와 샘플 History/재전송은 독립적인 문제다.
6. **실물 OPI5+ / 유·무선 LAN 검증을 완료한 다음 GitHub Issue와 병합 범위를 정리.** 현재 Issue를 완료 처리하지 않는다.

## 2. 대상 저장소 및 확인된 원격 커밋

| 구분 | 브랜치 | 확인된 HEAD | 상태 |
| --- | --- | --- | --- |
| Core | `integration/ux-20261124` | `8371acd33ffd` | TT1 커밋 포함 |
| Tools | `integration/ux-tools-core-20261124` | `2983f0542be4` | 이번 TT1 관련 추가 변경 없음 |

- Core TT1 커밋: [`8371acd`](https://github.com/Nyamkani/kss_control_framework/commit/8371acd33ffd3d5f2e8530982eaacd99ee34c023) — `feat(network): add opt-in topic timestamp tolerance`
- Core 최종 확인 CI: [Actions #45652402823](https://github.com/Nyamkani/kss_control_framework/actions/runs/45652402823) — **completed / success** (커밋 `8371acd`)
- Tools 최종 확인 CI: [Actions #45585610900](https://github.com/Nyamkani/kcf_tools/actions/runs/45585610900) — **completed / success** (커밋 `2983f05`)
- Core TT1 커밋의 변경 파일: `gateway/src/gateway_support.hpp`, `gateway/src/main.cpp`, `gateway/src/topic_rate_test.cpp`, `kcf/include/kcf/network/detail/topic_timestamp.hpp`(신규), `kcf/include/kcf/network/topic_data_plane.hpp`, `kcf/src/network/topic_data_plane.cpp`.
- 전체 변경량: 6개 파일, +369/-25 lines(원격 커밋 확인 기준).

**CI 범위 주의:** Tools의 성공 워크플로는 Core의 **TT1 이전 HEAD**를 기준으로 실행되었다. 최신 Core `8371acd` + Tools `2983f05` 조합의 별도 CI 재실행이 확인된 것은 아니다. 또한 GitHub에 반영된 커밋과 로컬 작업 디렉터리의 완전한 일치 여부는 별도 `git status` 확인 대상이다.

## 3. VM 검증 요약

| 단계 | 검증 내용 | 결과 | 증거 및 경계 |
| --- | --- | --- | --- |
| VM-1 | Core/Tools Debug configure 및 build | PASS | Ubuntu 24.04 VM, Core `build-ux`, Tools `build-ux-qt` |
| VM-2 | Core Gateway 정책/수명주기, Topic, Remote Action, Supervisor 실패·진단, CLI | 기능 PASS | 일부 100 Hz 측정에서 송신률 편차, 성능 한계 별도 기록 |
| VM-3 | Tools GUI/Backend/Codec/Network/Diagnostics | **9/9 PASS** | Real Fixture 타깃을 명시적으로 빌드한 뒤 실행 |
| VM-4 | 두 Host 격리 Virtual LAN 및 Tools Remote E2E | PASS | `namespace_isolation`부터 `overall`까지 사용자 실행 로그의 PASS 항목 확인 |
| VM-5 | 실제 Supervisor가 Publisher/Subscriber 자식 프로세스 기동·종료 | PASS(관측 범위) | Runtime/Endpoint 등록과 종료 후 0건 확인; 데이터 Sequence 로그 미제공 |

### 3.1 VM-4 Virtual LAN / Tools E2E

사용자 실행 명령은 다음과 같다.

```bash
cd ~/workspace/kcf/kcf_tools
mkdir -p build-ux-qt/remote-lan-e2e
sudo env QT_QPA_PLATFORM=offscreen \
  unshare --net --ipc --mount --pid --fork --mount-proc \
  python3 tests/remote_control_e2e.py \
  "$HOME/workspace/kcf/kss_control_framework" \
  "$PWD/build-ux-qt" \
  "$PWD/build-ux-qt/remote-lan-e2e"
```

관측된 PASS 항목:

- Namespace 격리, Generic Control/Topic UI, Gateway Session 변경, GUI 재설정
- Service Timeout의 `execution_unknown` 처리, **Timeout 이후 자동 Service 재실행 없음**(`timeout_service_executions = 1`)
- Remote Snapshot 선택·구독, 실제 Topic 수신, Remote Plot, CSV 준비, Sender Route 확인
- Metadata Revision 변경 중 신규 요청 차단 및 기존 Handle 보존, 완성 후 같은 Handle로 재수신
- Endpoint 등록 변경·삭제, Gateway LOST, Session 변경 시 기존 선택 무효화
- In-flight Stop 및 GUI 정리, overall PASS

**제약:** 격리된 Virtual LAN fixture 결과다. 실제 Wi-Fi/IGMP, ARM64, 장시간 부하와 systemd 운영 안정성을 대신하지 않는다. 테스트는 Simulator/Fixture 기반 원격 Control·Topic·GUI E2E이며, 일반 Application Supervisor를 통한 전체 실행 흐름과는 별개다.

### 3.2 VM-5 Supervisor / Element 실제 실행

`kcf_pubsub_bringup` 예제를 실행하고 별도 터미널에서 `kcf status`/`kcf topic list`와 OS 프로세스 트리를 관측했다.

```text
Application pubsub_example pid=471733 state=RUNNING elements=2
  Element publisher pid=471741 alive=1 failure=NONE
  Element subscriber pid=471742 alive=1 failure=NONE
Local runtimes=2, supervisors=1
/example/chatter SUB pid=471742
/example/chatter PUB pid=471741
Local Topic endpoints=2
```

부모/자식 관계: `bringup(471733)` → `publisher(471741)`, `subscriber(471742)`.

종료 후:

```text
Local runtimes=0, supervisors=0
Local Topic endpoints=0
```

**판정:** 실제 자식 프로세스 생성, RUNNING 인식, Topic Endpoint 등록, 종료 후 프로세스·등록 정리는 확인했다. 제공된 출력에는 Subscriber의 `[Subscriber] seq=...` 로그가 없으므로 **실제 Payload 수신/Sequence 증가 여부는 이번 VM-5 증거만으로 확정하지 않는다.** 또한 이번 시험은 동일 Host의 Local IPC 기반이며, Bringup이 직접 Gateway를 시작하는 시나리오가 아니다.

## 4. Gateway Topic 100 Hz — Known Limitation

### 4.1 현상 및 원인 관측

- Publisher와 Gateway가 독립적으로 주기를 실행한다. Gateway는 Local Topic의 `ReadLatest()`에서 동일한 Sample Sequence를 다시 볼 수 있으며, 중복 Sequence는 전송하지 않는다.
- Latest-only, no-catch-up 설계이므로 이전 샘플을 나중에 몰아서 전송하지 않는다.
- VM 프로세스 스케줄링 지연 때문에 일부 시험에서는 Publisher 자체도 목표 주기를 놓친다. `send_lateness`가 재계산된 deadline을 기준으로 할 경우 지나간 주기가 지표에서 가려질 수 있어 독립적인 missed-period 관측이 필요하다.
- 100 Hz Publisher / 100 Hz Gateway의 짧은 반복 시험에서는 전송률 변동이 관측되었으나, **125~200 Hz Publisher → 100 Hz Gateway** 조합에서는 대체로 100 Hz에 근접했다. 이는 phase/sample availability의 영향을 뒷받침하는 관측일 뿐, 네트워크 손실이 없다는 포괄적인 증명은 아니다.

### 4.2 TT1 6초 × 3회 측정 요약

| 모드 | Tolerance | Sender Hz (min / avg / max) |
| --- | --- | --- |
| Legacy | OFF | 99.88 / 99.98 / 100.03 |
| Legacy | 3 ms | 89.72 / 96.10 / 99.35 |
| Legacy | 5 ms | 98.72 / 99.03 / 99.38 |
| Fresh-start | OFF | 99.80 / 99.97 / 100.05 |
| Fresh-start | 3 ms | 96.18 / 97.93 / 99.74 |
| Fresh-start | 5 ms | 99.55 / 99.81 / 100.00 |

- TT1 5 ms 모드에서는 해당 반복의 100→100 Hz 전송률이 모두 95 Hz 이상이었지만 OFF도 거의 100 Hz였다. **TT1의 개선 효과가 일관되게 우수하다는 결론은 불가하다.**
- TT1의 추가 조회/recheck로 Worker 실행 비용이 소폭 늘어났다. 프로세스 CPU 수치는 Sender 단독 소비량이 아니므로 별도 측정값으로 취급해야 한다.
- 테스트 시점/VM 부하/스케줄 위상에 따라 결과가 달라졌다. hard real-time 또는 물리 Network 도착률 보장으로 해석하지 않는다.

## 5. TT1 — Topic별 Payload Timestamp Tolerance

### 5.1 구현 계약

- CLI 옵션: `--topic-timestamp-tolerance /topic:stamp:5000` (예: 5000 μs = 5 ms).
- Payload에 명시한 `uint64_t` Timestamp 필드를 사용하는 per-topic opt-in 방식.
- Timestamp 기준: **같은 Host에서** `CLOCK_MONOTONIC` nanoseconds, Publish 직전 생성 시각 계약.
- Gateway의 bounded recheck 및 오래된 동일 Sample 재전송 방지, Timestamp 오류·미래 시각·세대 변경 등 경계 조건 검증.
- 기본 **OFF**. 기존 Legacy(기본)/Fresh-start(선택) 정책, Latest-only, no-catch-up 유지.
- SHM/Wire 형식 변경 없음. 단 `TopicConfig` C++ 구조 변경을 사용하는 소비자는 재빌드 필요.

### 5.2 검증 및 주의

- Codex 보고 기준 Debug build PASS, 기능 회귀 11개 PASS, `git diff --check` PASS.
- Core 원격 CI에서 최종 커밋 `8371acd` 성공 확인.
- TT1이 Remote Host의 `CLOCK_MONOTONIC` 값을 Receiver의 시각과 직접 비교해 E2E absolute age를 산출할 수 **없다**. 각 Host의 monotonic epoch가 다르기 때문이다.
- 이번 검증에서 운영 기본값 OFF를 변경하지 않는다.

## 6. RR1 — Experimental Receiver Recovery 설계 검토

**상태:** 설계 검토 완료 / 구현 미실시 / R1~R10 SKIP.

| 누락 유형 | Receiver 보유 데이터만으로 복구 | 필요 조치 |
| --- | --- | --- |
| 늦은 역순 UDP 패킷 | 제한적 | 순서·중복·TTL 검사 및 보관 정책 |
| 이미 수신했으나 Latest로 덮인 샘플 | 사전 수집 시 가능 | 별도 History Buffer |
| 실제 UDP 유실 | 불가능 | Sender Cache, NACK/요청, 재전송, 기능 협상 |
| Gateway가 Publisher Commit을 건너뜀 | 불가능 | Sender 수집·보관 정책 확장 |
| Receiver의 SHM Publish 실패 | 최신 샘플 한정 재시도 가능 | 최신 데이터 폐기 정책 변경 |

확인된 현재 구조:

- Sender: `ReadLatest()` 사용, 전송 이력 캐시 없음.
- Receiver: `packet.sequence` Gap 통계, 최신 패킷 1개 보관, 이전 Sequence 거부.
- Proxy: SHM `depth=1`; 복구된 과거 Payload를 순차 재적용하면 기본 최신 상태 전달 정책과 충돌할 수 있음.
- `packet.sequence` Gap은 네트워크 전송 경로, `sample_sequence` Gap은 Publisher 샘플링 생략 가능성을 구분해야 함.
- Topic Wire v1은 INTEREST/DATA만 지원, 재전송 요청·협상 없음.

후속 후보:

1. **RR1-A 관측용 History:** Wire/SHM 변경 없이 별도의 bounded 수신 이력과 통계 제공. 기존 KCF Tools Plot History·Core 진단 기능과 **중복 여부 선검토**. 제어 Proxy에 과거 명령 자동 재적용 금지.
2. **RR1-B 실제 손실 복구:** Sender Cache, 요청/응답, 최대 재전송 횟수/TTL, 호환 협상, 구버전 fallback 등 별도 프로토콜 설계·승인 필요. 현재 보류.

메모리 예산 예시: 32 Route × 8 슬롯 × 최대 1200 B Datagram = 약 **300 KiB** + 관리정보(양방향 캐시 도입 시 별도 추가). 이는 설계 추정이며 구현 실측값이 아니다.

## 7. 후속 설계 — 공통 SHM Timestamp 표준화

현재 TT1은 각 Topic Payload에 Timestamp 필드가 있다는 계약에 의존한다. 장기적으로는 프레임워크가 SHM Metadata에 **Publish Timestamp, Sequence, Publisher/Session Generation 등**을 공통 제공하는 쪽이 더 일관적이다.

그러나 다음을 명확히 분리한다.

- **공통 SHM Timestamp** → 같은 Host의 Publish 시각 및 IPC Age 관측 표준화.
- **Remote Host 간 시각 비교** → clock-domain 동기화/변환 또는 별도 수신 시각 계약 필요.
- **History/Recovery** → 별도 Buffer, Sender Cache 및 전송 정책 필요.
- **100 Hz Phase/Timing** → Scheduler 및 Sample availability 문제로 독립 검증 필요.

SHM 포맷, Publisher/Reader/Proxy, ABI, 마이그레이션 및 기존 binary 재빌드 범위를 검토한 뒤에만 IPC 포맷 개정을 결정한다. 현재 구현 지시나 API 확정은 아니다.

## 8. 실물 테스트 계획 (Issue 정리 전 선행)

대상: 실제 OPI5+ ARM64 + 개발 VM Tools, 유선 Ethernet 및 Wi-Fi를 구분해 시행한다.

| ID | 검증 항목 | 합격/관측 기준 | 상태 |
| --- | --- | --- | --- |
| P-01 | Core ARM64 빌드 및 실행 | 현재 통합 코드와 TT1 포함 버전/커밋 확인, 빌드 및 시작 | PENDING |
| P-02 | systemd Host Gateway | 기존 Gateway 소유권, singleton, 서비스 재시작, Application 종료 시 유지 | PENDING |
| P-03 | Local Element / Bringup | UGV02 또는 동등 Element의 정상 기동·Topic·종료·잔여 PID/SHM | PENDING |
| P-04 | 유선 Remote Discovery/Topic | Host/Endpoint READY, 실제 Payload, 50/100 Hz 측정 | PENDING |
| P-05 | Wi-Fi Discovery 회복 | ONLINE/LOST 및 복구 지연, 양단 패킷/IGMP 타임라인 | PENDING |
| P-06 | TT1 기본 OFF/선택 ON | 기능·속도·CPU/지터 비교, remote absolute age로 오인 금지 | PENDING |
| P-07 | 장시간·복구 | Gateway/Tools 재시작, Session/Revision 변경, Topic STALE, 누락·오류 통계 | PENDING |
| P-08 | Tools Plot/CSV | 실제 UGV02 수치값 Plot/CSV, Host/Route/시각 출처 확인 | PENDING |
| P-09 | 안전 정책 | Remote Control default deny, 오작동·stale 명령 처리, Watchdog 경계 | PENDING |

실물 테스트 표는 **실행 전 계획**이며 아직 PASS가 아니다. Wi-Fi Discovery의 기존 4~6분 복구 지연은 원인 미확정 상태로 추적한다. Network Topic 100 Hz 이슈를 단순 평균 수신률 하나로 종료하지 않는다.

## 9. 실물 시험 후 GitHub Issue 정리 대상

아래 이슈는 현재 상태의 정리 후보일 뿐이며, 이 문서 작성으로 수정/종료하지 않는다.

| 저장소 / Issue | 현재 추적 사항 | 실물 테스트 후 판단 |
| --- | --- | --- |
| [Core #6](https://github.com/Nyamkani/kss_control_framework/issues/6) | Wi-Fi Discovery LOST·복구 지연 | P-05 패킷/복구 시간 확인 후 원인·진척 업데이트 |
| [Core #7](https://github.com/Nyamkani/kss_control_framework/issues/7) | Gateway standalone/선택적 Supervisor 통합 | Host systemd 기본 운영과 범용 관리 API 미구현 구분 |
| [Core #10](https://github.com/Nyamkani/kss_control_framework/issues/10) | Supervisor 다중 실패 진단 | 외부 조회·실물 안정성 및 수용 기준 확인 |
| [Core #12](https://github.com/Nyamkani/kss_control_framework/issues/12) | Topic 공개 정책 | default/exclude/권한/실물 동작 확인 |
| [Core #13](https://github.com/Nyamkani/kss_control_framework/issues/13) | v5.6.1 실물 운영 회귀 | P-01~P-05, 패키지/Runtime 포함 검증 후 업데이트 |
| [Core #15](https://github.com/Nyamkani/kss_control_framework/issues/15) | CLI 관리·진단 명령 | 구현된 slice와 미구현 명령 구분 |
| [Core #16](https://github.com/Nyamkani/kss_control_framework/issues/16) | Topic Rate/Loss/Freshness 진단 | 로컬/네트워크 계측 구분, 100 Hz 제한 첨부 |
| [Core #17](https://github.com/Nyamkani/kss_control_framework/issues/17) | 구조화된 오류 진단 | 진단 단계별 범위·Unknown 처리 확인 |
| [Core #20](https://github.com/Nyamkani/kss_control_framework/issues/20) | Remote Action Event/Poll | 실제 부하/latency/CPU 및 policy 확인 |
| [Tools #4](https://github.com/Nyamkani/kcf_tools/issues/4) | Plot/CSV 및 이력 | 실제 센서와 장시간·다중 Plot 검증 후 업데이트 |
| [Core #9](https://github.com/Nyamkani/kss_control_framework/issues/9), [Tools #3](https://github.com/Nyamkani/kcf_tools/issues/3) | Manifest/Launch Designer | 이번 Release 범위 밖, OPEN 유지 |

추가 기록 후보(기존 이슈와 중복 여부 확인 후 판단): **Network Topic 100 Hz Rate Stability**, **공통 SHM Timestamp / Clock Domain 계약**, **RR1 재전송 설계 보류**. 실물 시험 전 신규 Issue 생성은 하지 않는다.

## 10. 최종 상태 및 반영 방침

- **완료:** Core/Tools VM 기능 및 Virtual LAN E2E, Supervisor/Element 실행·정리 관측, TT1 기능 구현과 Core 최신 CI, RR1 구조 검토.
- **보류:** RR1 실제 재전송, SHM Timestamp 포맷 개정, 물리 Wi-Fi 원인 확정, 장시간·실물 성능 보장.
- **운영 정책:** Topic 최신값 우선, no-catch-up, Gateway 노출/제어 권한 기본 정책 유지, TT1 기본 OFF.
- **GitHub:** 문서 초안은 별도로 작성했으며 저장소 파일 수정/Issue 갱신/PR merge/Release 작업을 실행하지 않음.
- **다음 단계:** 실물 OPI5+ 및 유선·Wi-Fi 검증 결과 수집 → 표/한계 업데이트 → Issue별 수용 조건 재평가 및 명시적 승인 후 정리.

### 검증 근거

- 사용자가 제공한 VM-4 `remote_control_e2e.py` 터미널 출력 (`overall: PASS` 등).
- 사용자가 제공한 VM-5 `kcf status`, `kcf topic list`, OS PID/PPID 및 종료 후 0건 출력.
- Codex가 보고한 TT1 기능 및 6초 반복 성능 측정, RR1 단계별 설계 검토.
- GitHub의 Core/Tools 통합 브랜치·커밋·CI 상태 및 기존 Issue 본문 조회(2026-11-26).

**증거 수준:** 사용자 로그, Codex 측정 결과, 원격 GitHub CI를 각각 구분하며 물리적 장치 검증으로 확대 해석하지 않는다.