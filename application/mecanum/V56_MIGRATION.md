# KCF Core v5.6.0 마이그레이션 — 2026-10-29

## 기준과 보존

- 작업 브랜치: `mecanum`, 시작 HEAD `c6b8f78b5bfdec036e63cc1508059da5d2246fad`.
- 시작 작업 트리는 추적·미추적 변경 없이 깨끗했음. stash/reset/clean, 새 브랜치·Worktree 생성 없음.
- 기준: 로컬 `dev` 저장소에서 확인한 `570a233711c2415bbf9acba273295458d494cfca`.
- 로컬 fetch 후 `git merge --no-ff --no-commit 570a233711c2415bbf9acba273295458d494cfca` 수행.
- Core(`kcf/`)와 Bringup(`bringup/`)은 기준 커밋과 일치. NetworkProtocol/Discovery/Topic/IPC 버전은 upstream v5.6 계약을 그대로 사용.
- Motor·IMU·EKF·Keyboard 구현, 메시지, UART, MCU 펌웨어, 기존 테스트 소스는 시작 HEAD와 동일. 알고리즘·250ms timeout 변경 없음.
- 기존 Application 전체 파일과 과거 검증 기록 보존. 전용 Gateway 소스도 보존하고 빌드 등록만 공통 `kcf_gateway`로 교체.

## 충돌 해결

21개 충돌 파일을 각각 확인했다. 루트 CMake를 제외한 아래 20개 파일은 `mecanum` HEAD 내용이 기존 v5.5 기준 `f184787`과 바이트 단위로 동일했다. Mecanum 고유 수정이 없음을 확인한 뒤 v5.6 파일을 반영했다. 무조건적인 ours/theirs 일괄 선택을 하지 않았다.

- `Changelog.md`, `README.md`
- `docs/06_VERIFICATION_AND_LIMITATIONS.md`, `docs/10_NETWORK_DEPLOYMENT_POLICY.md`
- `examples/network/CMakeLists.txt`, `examples/network/LAN_TEST_GUIDE.md`
- `examples/network/src/peer_discovery_test.cpp`, `remote_control_client_test.cpp`, `topic_data_test.cpp`
- `examples/supervisor/CMakeLists.txt`, `kcf/CMakeLists.txt`
- `kcf/include/kcf/network/compatibility.hpp`, `control_node.hpp`, `local_metadata.hpp`, `remote_control_client.hpp`, `remote_topic_client.hpp`, `topic_data_plane.hpp`
- `kcf/src/network/control_node.cpp`, `local_metadata.cpp`, `topic_data_plane.cpp`

루트 `CMakeLists.txt`는 버전 5.6.0과 공통 Gateway 등록을 적용하면서 Motor/IMU/EKF/Keyboard의 네 Application 등록을 모두 보존했다. 해결되지 않은 충돌 없음.

## Gateway 정책 비교

| 항목 | 기존 전용 Gateway | v5.6 운영 구성 |
|---|---|---|
| 실행 | `kcf_mecanum_gateway`, ProcessRuntime | 공통 `kcf_gateway`, 기존 공통 생명주기·네트워크 복구 |
| Host ID | `--host-id` | `--state-dir/host-id` 영속 UUID; 기존 ID 이관 가능 |
| NIC | `--local-ip` | `--interface`, 공통 선택·복구 기능 |
| 관찰 모드 | `--tools-host-id` | 정확한 `--remote-topic` 5개 + `--descriptor-client` 1개 + `--diagnostics` |
| Descriptor 권한 | 지정 Tools ID | 지정 Tools ID만; 공개 observer를 켜는 prefix 옵션은 사용하지 않음 |
| Diagnostics | Tools 모드 ON | upstream에 CLI가 없어 `--diagnostics`만 최소 추가; 기본 OFF |
| 전송 | UDP 37652, 기본 50Hz | 동일 |
| Discovery/Control | 37651 / 39001 | 동일 |
| 제어 | cmd_vel 비공개, 제어 권한 없음 | 동일; Parameter Get/Set·Service·Action 허용 인자 없음 |
| 메타데이터 | 모든 로컬 runtime, Supervisor 관계 | 동일; runtime_pids 제한 없음 |
| 중복 실행 | 전용 잠금 없음 | 공통 Gateway의 동일 경로 잠금으로 거부 |

공통 Gateway 변경은 `gateway_support.hpp`의 Diagnostics 옵션 전달, `main.cpp`의 도움말·로그, `gateway_test.cpp`의 관측 구성 회귀에 한정한다. Core Network API는 추가 변경하지 않았다.

운영 명령은 [Bringup README](bringup/README.md)에 기록했다. 이전 빌드에 남은 전용 바이너리는 자동 삭제하지 않았다. 해당 바이너리는 공통 잠금을 사용하지 않으므로 운영에서 함께 실행하면 안 된다. 공통 Gateway도 서로 다른 lock-file을 지정하면 별도 잠금 영역이므로 동일 Host에서는 같은 운영 계정·잠금 경로를 사용해야 한다. 원격 권한은 신뢰 LAN 정책이며 인증 기능으로 간주하지 않는다.

## 소프트웨어 검증

새 Debug 빌드 디렉터리 `/tmp/mecanum-v56-debug` 사용:

```sh
cmake -S . -B /tmp/mecanum-v56-debug -DCMAKE_BUILD_TYPE=Debug \
  -DMECANUM_BUILD_TESTS=ON -DMECANUM_BUILD_IMU_TESTS=ON -DMECANUM_BUILD_EKF_TESTS=ON
cmake --build /tmp/mecanum-v56-debug -j4
```

| 검증 | 결과 |
|---|---|
| Debug configure, 전체 빌드(모든 Mecanum·공통 Gateway 포함) | PASS |
| Motor / Firmware / IMU / EKF 기존 단위 테스트 | 각 PASS |
| Gateway 기본 설정·잠금·Mecanum 관측 구성 | PASS |
| Gateway lifecycle / Topic policy / local IPC 기본 테스트 | 각 PASS |
| 공통 Gateway `--help` | PASS |
| 루프백 Discovery-only / 5개 Topic + Diagnostics 시작·SIGTERM 종료 | 각 PASS |
| 공통 Gateway 두 번째 프로세스 잠금 거부 | 두 모드 모두 PASS |
| 잘못된·자기 Host ID 및 포트 충돌 거부 | PASS |
| 기존 PTY Motor·IMU·EKF 통합 | PASS |
| Bringup 시작·독립 Publisher·EKF 수신·입력 중단 무효화·순차 복구 | PASS |
| EKF/입력 프로세스 재시작·SIGKILL 오류·미연결 UART 오류·정상 종료와 자식 회수 | PASS |
| 기존 PTY 비영점 직진·횡이동·회전 흐름과 만료 정지 | PASS |
| Keyboard W/S/A/D/Q/E, Space/Esc, 입력 중단, SIGINT/SIGTERM, SIGKILL timeout | PASS |
| Keyboard 중복 Publisher·Supervisor 독립성·관리 실행 거부 | PASS |
| 기존 Publisher 재생성 후 Motor 자동 재연결 제한 재현 | PASS(제한은 그대로 유지) |
| `git diff --check HEAD`, 충돌 marker/미해결 index 확인 | PASS |

PTY 비영점 시험의 명령 생성 중단 후 정지 요청 관측은 직진 251.079ms, 횡이동 261.321ms, 회전 254.874ms였다. Keyboard 입력 중단은 173.825ms, SIGKILL 후 마지막 비영점 생성 시각 기준 Motor 정지 요청은 254.998ms였다. 이번 소프트웨어 실행의 관측값이며 실시간·물리 정지 보장이 아니다.

로그: `/tmp/mecanum-v56-configure.log`, `/tmp/mecanum-v56-build.log`, `/tmp/mecanum-v56-results/`. PTY 통합에는 현재 설치된 KCF Tool의 기존 Local backend를 v5.6 Core와 다시 링크한 `/tmp/v56-tool-probe`를 사용했다. Tool 소스 변경 없음. 보조 `cmd_watch`, `command_producer`도 동일 Core로 다시 빌드했다.

빌드에는 기존 WIT SDK의 signedness/fallthrough 경고 및 upstream Topic DataPlane 시험 빌드의 indentation 경고가 남아 있다. 경고를 숨기거나 무관한 코드를 수정하지 않았다.

## 미검증 및 유지된 한계

- 실제 RPi5 UART, 모터·IMU 실측, MCU watchdog·물리 정지·로봇 주행 및 PC↔RPi5 물리 LAN: 미실시.
- systemd 설치·설정·서비스 실행: 미실시. 네트워크 장애 복구는 기본 생명주기 시험 범위만 확인했고 실제 NIC 단절·주소 변경·LAN QoS를 검증하지 않았다.
- 이번 Gateway 실행 시험은 루프백 설정·생명주기·잠금이다. 5개 실제 센서 Topic의 LAN 수신·Diagnostics UI 및 descriptor 접근 제어의 PC↔RPi5 종단 검증은 미실시.
- 기존 Motor의 cmd_vel Publisher 재생성 자동 재연결 제한, EKF 입력 공백 중 이동에 따른 위치 오차 등은 유지한다.
- 과거 v5.1/v5.5 검증 문서·결과 파일은 Historical 자료로 보존했다. v5.6 소프트웨어 적용 완료와 RPi5 재검증 완료는 별개다.
