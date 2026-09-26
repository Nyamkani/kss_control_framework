# Mecanum 통합 소프트웨어 검증 결과

실행일: 2026-09-21. Linux 일반 프로세스 환경, PTY 2개, 합성 Encoder·IMU 입력. 물리 UART·모터·보드는 사용하지 않았습니다. 실행 설정과 재현 명령은 [실행 안내](../../bringup/README.md)에 있습니다.

## 변경 파일

- `application/mecanum/bringup/run.py`: 기존 Bringup 명령행 기반 실행 구성.
- `application/mecanum/bringup/README.md`: 실행 인자·연결 계약·재시작 범위.
- `application/mecanum/tests/integration/run_integration.py`: PTY와 실제 프로세스 통합 검증.
- `application/mecanum/tests/integration/tool_probe.cpp`: 기존 Tool 백엔드를 호출하는 관찰 프로그램.
- 이 검증 결과 문서.

Motor·IMU·EKF 내부 로직, Arduino 펌웨어, KCF Core, Tool 소스 및 CMake는 수정하지 않았습니다. 기존 실행파일을 대상으로 빌드했고 검증 관찰 프로그램만 별도로 컴파일했습니다.

## 기본 프로세스 구성 및 IPC

단일 `kcf_bringup`이 세 자식을 실행하고, 감독 상태가 모두 RUNNING임을 확인했습니다. 실제 조회 시 PID는 Motor 1149692, IMU 1149694, EKF 1149695였습니다. 세 프로세스 모두 SUPERVISED이며, 각 Topic의 발행자 PID가 해당 실행파일의 PID와 일치했습니다.

실행 접두사: `/mecanum_integration_1149685`.

| Topic 접미사 | 실제 descriptor | 읽은 필드 수 | Tool 읽기 결과 |
|---|---|---:|---|
| /wheel_state | mecanum.WheelState.v1 | 12 | 성공 |
| /wheel_odometry | mecanum.WheelOdometry.v1 | 11 | 성공 |
| /imu/raw_data | kcf.mecanum.ImuSample | 10 | 성공 |
| /imu/mag_raw | kcf.mecanum.MagRawSample | 5 | 성공 |
| /ekf/odometry | kcf.mecanum.Ekf2D.v1 | 29 | 성공 |

초기 관찰에서 EKF `valid=true`, 입력 세대는 각각 1, 출력 sequence는 112였습니다. 이후 sequence 증가도 확인했습니다. 가상 Motor는 `m/r/e/z`에 응답했고, IMU 합성 프레임은 실제 SDK·파서를 통과하여 별도 프로세스에서 발행되었습니다. Topic 메시지 대신 검증 프로그램에서 직접 필터 함수를 호출하는 방식은 사용하지 않았습니다.

## 수명주기·복구 결과

| 항목 | 결과 |
|---|---|
| 단일 Bringup 시작 장벽 | 세 프로세스 RUNNING, 성공 |
| IMU 프레임 공급 중단 | 프로세스는 유지, EKF `valid=false` 확인 |
| IMU 프레임 공급 재개 | EKF `valid=true` 복구 |
| 단일 Bringup 정상 종료 | 종료코드 0, 자식 3개 회수, 강제 종료 없음 |
| EKF 단독 재시작 | 입력 두 PID 유지, EKF 새 세션으로 정상 수신 |
| IMU 정상 종료·재시작 | EKF 세션 유지, `imu_generation: 1→2`, 무효→유효 |
| Motor 정상 종료·재시작 | EKF 세션 유지, `wheel_generation: 1→2`, 무효→유효 |
| 입력별 재시작 후 재연결 수 | `reconnects: 0→1→2` |
| IMU SIGKILL | 해당 Bringup ERROR, EKF `valid=false`, 오류 감독자 종료코드 1 |
| 없는 UART 경로 | 예상 초기화 실패, 비영 종료코드 |
| 가상 Motor 명령 | 목표 RPM 명령 33개 모두 네 바퀴 0 |

개별 재시작 단계는 **요소별 Bringup 세 개**를 사용했습니다. 단일 감독자의 개별 자식 재시작 API가 없으므로, 해당 기능이 있다고 주장하지 않습니다. 입력별 재시작 동안 EKF 세션 `370206982277998`이 유지됐습니다. 물리 정지 확인이 아니라 가상 UART ACK 및 프로세스 수명주기 검증입니다.

## Tool 검증 범위

설치된 Tool 저장소의 수정하지 않은 `KcfBackend`와 `value_codec`를 연결해 실제 `Refresh/GetElements/StartTopicEcho/ReadTopicEcho` 경로로 상태·descriptor 기반 데이터를 확인했습니다. 재시작 이후에는 새 조회와 Echo 세션으로 데이터를 다시 읽었습니다. 실제 GUI 화면의 표시·조작 및 기존 장기 Echo 세션의 자동 재연결은 검증하지 않았습니다. 그래프·이력 기능은 추가하지 않았습니다.

## 환경과 한계

초기 격리 실행에서는 세 프로세스가 Element Setup 전에 `ProcessRuntime::StartSupervision()`의 Unix 감독 소켓 검사에서 `-EPROTO`로 종료됐습니다. 같은 바이너리·실행 구성을 일반 프로세스 환경에서 실행한 최종 검증은 13개 검사항목 모두 통과했습니다. 격리 환경의 구체적인 소켓 호출 차이까지는 확정하지 않았고 Core를 수정하지 않았습니다.

관찰 도구의 상태 문자열, 비동기 런타임 등록 시차, `wheel_generation` 필드명에 맞춰 테스트의 판정을 보완했습니다. 필터·드라이버 결함을 수정한 것은 아닙니다.

재실행 가능한 최종 원시 결과는 `/tmp/mecanum-integration-final/`의 JSON·감독 로그와 `/tmp/mecanum-integration-final.log`에 저장했습니다. 테스트는 생성한 PTY와 고유 접두사의 Topic만 정리하며, 다른 Application의 자원에는 신호를 보내거나 삭제하지 않습니다.

실기 미검증: 실제 UART 부하·단절·보드 재부팅, 모터 정지·watchdog, 센서 장착과 방향 기준, 실제 궤적 정확도. 입력 중단 중 실제 이동이 있었다면 복구 이후에도 위치 오차가 남을 수 있으며, 이번 결과는 이를 복원하거나 위치 정확도를 보장하지 않습니다. cmd_vel 발행자 교체에 대한 Motor 구독 재연결과 오래 열린 Tool Echo의 자동 복구도 이번 검증 범위 밖입니다.

최종 범위 제한 재검증: 테스트 고유 Topic의 발행자 PID만 선택하도록 제한한 후에도 13개 항목 모두 통과했습니다. 접두사는 `/mecanum_integration_1215352`, 초기 PID는 `{'kcf_mecanum_ekf': 1215381, 'kcf_mecanum_imu': 1215380, 'kcf_mecanum_motor': 1215379}`입니다. 목표 RPM 명령 30개는 모두 0이었습니다. 이 최종 실행 기록은 `/tmp/mecanum-integration-scoped/`와 `/tmp/mecanum-integration-scoped.log`에 있습니다.
