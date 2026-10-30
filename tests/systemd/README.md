# STAB-6B: systemd 운영 시험 준비

Framework **5.5.0**, Tools **0.14.0**을 유지한다. 이 패키지는 설치 도구가 아니다.
기본 실행은 실제 서비스 제어 없이 Unit 렌더링, 계약 검사, `systemd-analyze verify`,
실행 계획 및 결과 파일 생성만 수행한다. 준비 PASS를 운영 PASS로 해석하지 않는다.
제품 Template은 [기존 Unit](../../gateway/systemd/kcf-gateway.service.in)을 사용한다.
[서비스 UID 계약](../../gateway/systemd/README.md), [Local IPC v1](../../gateway/LOCAL_IPC.md)을 따른다.

## 기본 준비 / Dry-run

기존 Debug 빌드의 Gateway가 필요하다. 시험 계정은 실제 존재하는 **비root 계정**이며
Gateway와 Local Runtime을 같은 UID로 실행한다. 실행 파일의 절대 경로는 공백, `$`, `%`가
없는 경로를 사용한다. 시험 계정이 해당 바이너리와 상위 디렉터리에 접근 가능해야 한다.

```bash
python3 tests/systemd/run_systemd_test.py \
  --gateway /absolute/debug/gateway/kcf_gateway \
  --user testuser \
  --output-dir /tmp/kcf-stab6b-prepare
```

`--framework-root`는 기본적으로 이 스크립트가 속한 저장소다. 출력 디렉터리는 비어 있어야
한다. 기존 결과를 덮어쓰지 않는다. 성공 시 종료 코드 0, FAIL 시 1을 반환한다.

생성물:

- `kcf-gateway-stab6b.service`: 렌더링한 시험 Unit
- `PLAN.md`, `RESULT.md`, `results.json`
- `*-systemd-analyze.log`: 명령과 최초 stdout/stderr
- 실제 실행 모드에만 `systemctl show`, journal, 중복 실행 및 Runtime 로그
- 실제 실행 모드의 JSON: IPC Host/Boot/Session, PID, FD 및 Worker 개수, 실행 시간·종료 코드

실제 Gateway 시작 후 Session 로그와 IPC Session을 비교한다. Native 구조체 전송이나
새 IPC Protocol은 사용하지 않는다. root 시험 드라이버도 IPC 조회 때는 서비스 UID로
effective UID를 낮춰 기존 `SO_PEERCRED` 계약을 따른다.

## 격리 및 승인 조건

**이번 준비 작업에서는 아래 운영 명령을 실행하지 않는다.**

실행할 수 있는 곳은 운영 RPi5/PC가 아닌, 관리자가 폐기 가능하다고 확인한 systemd VM 또는
컨테이너다. VM 감지 자체는 안전성 증명이 아니다. 승인 문구와 root 소유 표시 파일은
관리자가 해당 환경을 직접 확인했다는 추가 조건이다. VM/컨테이너도 운영 중이면 금지한다.
컨테이너는 PID 1 systemd, 사용 가능한 cgroup 관리, 동일 PID/SHM 가시성이 필요하다.
호스트의 `/run`, `/var/lib`, `/dev/shm`, systemd 소켓을 공유하는 컨테이너는 사용하지 않는다.

스크립트는 sudo를 실행하지 않는다. 실제 실행에는 관리자가 승인한 guest 내부 root 셸이
필요하며, 다음 조건 중 하나라도 없으면 설치·제어 전에 거부한다.

- `--execute`와 정확한 `--approve-disposable-environment I_APPROVE_DISPOSABLE_SYSTEMD_TEST`
- `--guest-marker`: 내용 `KCF_STAB6B_DISPOSABLE_GUEST` + 줄바꿈, root 소유 regular file,
  symlink 아님, group/world 쓰기 불가
- PID 1이 systemd이며 `systemd-detect-virt`가 VM/컨테이너를 식별
- 기존 `kcf_gateway` 프로세스가 없음
- 시험 Unit과 시험 State/Runtime 경로가 존재하지 않음
- 빌드된 `kcf_gateway_test` fixture 제공

시험 자원은 고정되어 있으며 운영 경로와 분리된다.

| 항목 | 시험 값 |
|---|---|
| Unit | `/run/systemd/system/kcf-gateway-stab6b.service` |
| StateDirectory | `/var/lib/kcf-stab6b` |
| RuntimeDirectory | `/run/kcf-stab6b` |
| Lock / IPC | 시험 RuntimeDirectory의 `gateway.lock` / `gateway.sock` |
| Discovery | UDP 47661 |
| Control / Metadata | TCP / UDP 49011 |
| Topic | UDP 47662 (공개 정책 없음, 기본 OFF) |
| NIC | `lo`, 명시적 `--allow-loopback` |

`/etc/kcf/gateway.env`를 읽지 않으며 `$KCF_GATEWAY_ARGS`도 사용하지 않는다.
RestartSec=3, StartLimitIntervalSec=60, StartLimitBurst=5 등 운영 Template의 정책은
유지한다. Local Runtime fixture는 Unit cgroup 밖에서 동일 UID로 실행한다.
기존 `/gateway/recovery/owner`, `/gateway/recovery/local-topic` 테스트 자원을 사용하는
다른 시험이 없는 전용 guest에서 직렬 실행한다.

## 승인된 guest에서 Lifecycle 자동 시험

아래는 **향후 실행 절차**다. 기존 서비스나 실제 장치에서 실행하지 않는다.
관리자가 guest 안에 비root `testuser`와 바이너리를 준비하고, 승인 후 표시 파일을 만든다.

```bash
# 승인된 일회용 guest의 root 셸에서만
printf 'KCF_STAB6B_DISPOSABLE_GUEST\n' > /root/kcf-stab6b-approved
chmod 600 /root/kcf-stab6b-approved
python3 tests/systemd/run_systemd_test.py \
  --gateway /absolute/debug/gateway/kcf_gateway \
  --runtime-fixture /absolute/debug/gateway/kcf_gateway_test \
  --user testuser --output-dir /tmp/kcf-stab6b-live \
  --execute \
  --approve-disposable-environment I_APPROVE_DISPOSABLE_SYSTEMD_TEST \
  --guest-marker /root/kcf-stab6b-approved
```

자동 시험 순서:

1. 별도 Runtime/Publisher 시작 → 시험 Unit 시작 → IPC RUNNING / nonzero Session.
2. main PID SIGKILL → systemd 재시작 → PID/Session 변경, Host ID 유지.
3. 로그와 IPC 일치, 이전 PID 소멸, FD/Worker 수 비교, 독립 Runtime PID 생존.
4. 동일 UID·동일 Lock의 중복 Gateway 실행이 `-EBUSY`로 거부되는지 확인.
5. SIGTERM → inactive/MainPID=0; RestartSec보다 긴 관측 후 재시작 없음.
6. RuntimeDirectory 및 Lock inode 보존, 두 차례 Start/Stop 및 socket 회수.
7. 시험 Unit에만 잘못된 인자 적용 → exit 2, NRestarts=0.
8. 원래 시험 Unit 복구 → 제한 내 연속 SIGKILL → `Result=start-limit-hit`.
9. 독립 Runtime 정상 종료와 Local Topic Publish/Read 연속 검증 로그 확인.
10. 최초 실패를 유지하고 journal/show를 수집; 자신이 생성한 Unit·디렉터리만 정리.

4초 관측은 제품 Timeout 증가가 아니라 RestartSec=3 이후 자동 재시작 부재를 확인하는
시험 창이다. 조건 대기는 유한하다. 실패를 재시도로 숨기지 않는다.
FD/Worker 비교는 수량 검사이며 모든 종류의 장기 누수 부재를 증명하지 않는다.

스크립트가 SIGKILL되거나 guest가 중단되면 자동 정리가 보장되지 않는다. 재실행은 남은
시험 경로를 거부한다. 관리자가 journal, 소유 PID, Lock 상태를 보존·확인한 뒤 **시험
Unit과 시험 디렉터리만** 수동 정리한다. `/dev/shm` 전체 삭제는 금지한다. Fixture 강제
종료 시 SHM이 남을 수 있으며 스크립트가 다른 소유자의 SHM을 삭제하지 않는다.

## Network Recovery / Metadata: 별도 수동 시험

이 부분은 자동 Lifecycle 실행 결과와 구분하여 SKIP으로 남는다. 승인된 disposable guest에서
관리자가 렌더링한 시험 Unit을 검토·배치하고 `--interface kcf6btest0`로 변경한다
(`--allow-loopback` 제거). 기존 인터페이스와 이름이 충돌하면 중단한다.
`ip -d link`로 새로 만든 **dummy NIC**임을 확인하고, 실제 NIC는 절대로 내리지 않는다.

1. dummy NIC가 없는 상태에서 시작: IPC WAITING_NETWORK, Session=0.
2. `ip link add kcf6btest0 type dummy`; guest에 충돌 없는 테스트 주소
   `192.0.2.1/24` 부여, multicast on/up → RUNNING, Session 기록.
3. 해당 dummy NIC만 down/up → WAITING/REBINDING/RUNNING, 새 Session.
4. 같은 dummy의 주소만 `192.0.2.2/24`로 변경 → 새 Session, Host/Boot 유지.
5. 각 단계에서 GET_STATUS, journal, systemctl show, PID/FD/Worker 수 저장.
6. 같은 UID의 독립 Runtime/Publisher는 종료하지 않고 Local Read가 계속되는지 확인.
7. 별도 기존 Discovery observer를 시험 포트에 연결하여 Local Runtime Endpoint의
   `runtime.pid/start_ticks`, `registration_id`, metadata_ready/revision을 관측한다.
   GET_PEERS는 **remote peer** 목록이므로 Local Endpoint 재발견 증거로 대체하지 않는다.
8. 원격 Topic consumer가 있다면 이전 Session/Route 선택이 무효화되는지 확인하고,
   명시적으로 새 세대를 선택한다. 자동 reattach/retry를 기대하지 않는다.
9. 시험 Unit 종료 후 자신이 만든 dummy만 삭제한다. 호스트 NIC·방화벽은 변경하지 않는다.

Endpoint/Route 재발견은 자동 스크립트에 구현되지 않았으며, 위 수동 로그가 없으면 SKIP이다.
실제 물리 LAN, Network 100Hz 성능 및 로봇 Fail-safe의 검증을 대신하지 않는다.

## Boot: 일회용 VM에서만 별도 검증

자동 스크립트는 enable/reboot하지 않는다. `/run/systemd/system` Unit은 재부팅 때 사라지므로
Boot 시험에는 승인된 VM 안에서만 렌더링한 **시험 Unit**을 `/etc/systemd/system/`에 배치한다.

- 관리자가 시험 Unit만 enable하고 VM reboot (운영 `kcf-gateway.service`와 무관).
- 부팅 후 `systemctl show kcf-gateway-stab6b.service`,
  `journalctl -b -u kcf-gateway-stab6b.service`, `systemd-analyze critical-chain` 저장.
- network-online.target 이후 시작 관계와 실제 IPC RUNNING을 각각 확인.
  network-online.target은 특정 NIC/주소의 사용 가능성을 보장하지 않는다.
- 별도 부팅에서는 시험 dummy NIC를 늦게 준비해 WAITING → RUNNING 복구 확인.
- 시험 완료 후 시험 Unit만 disable/stop하고 해당 파일·시험 경로만 정리.

재부팅·systemd guest가 없으면 명확히 SKIP. Static verify 또는 namespace 테스트 PASS를
systemd 부팅/재시작 검증 PASS로 기록하지 않는다.
