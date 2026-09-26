# Mecanum 프로세스 통합 실행

`run.py`는 기존 `kcf_bringup` 명령행을 구성한 뒤 `exec`합니다. 새 감독 API나 자동 재시작 기능은 추가하지 않습니다. Motor·IMU·EKF 및 Core 내부 코드는 변경하지 않았습니다.

## 빌드와 실행 설정

저장소 루트에서 기존 실행파일만 빌드합니다.

```sh
cmake -S . -B /tmp/kcf-mecanum-ekf-build
cmake --build /tmp/kcf-mecanum-ekf-build --target kcf_bringup kcf_mecanum_motor kcf_mecanum_imu kcf_mecanum_ekf -j2
```

실행 형식은 다음과 같습니다. UART 두 경로는 필수이며 실제 보드에 대한 기본 경로를 자동 선택하지 않습니다. 아래는 가상 터미널 예시이며, 자동 검증에서는 테스트가 생성한 경로를 직접 전달합니다.

```sh
python3 application/mecanum/bringup/run.py \
  --build-dir /tmp/kcf-mecanum-ekf-build \
  --motor-serial /dev/pts/10 --imu-serial /dev/pts/11 \
  --topic-prefix /mecanum --application-name mecanum
```

`--motor-baud`, `--imu-baud` 기본값은 각각 115200입니다. `--imu-yaw-relative true|false`의 기본값은 `true`이며 EKF에 전달합니다. Motor 명령 유효기간 250밀리초, IMU 축 설정, EKF 측정 선택·유효기간·Q/R은 기존 실행파일 기본값을 사용합니다. 별도 설정 저장소는 없습니다.

기본 `--component all`은 단일 Bringup이 다음 세 프로세스를 소유합니다. 기존 Bringup 명령행은 자동 이름을 부여하므로 Element 이름은 아래와 같습니다. 실행파일이 지원하지 않는 `--element-name`을 전달하지 않습니다.

| Bringup 이름 | 실행파일 | 입출력 |
|---|---|---|
| element1 | kcf_mecanum_motor | UART, cmd_vel 구독, wheel_state·wheel_odometry 발행 |
| element2 | kcf_mecanum_imu | UART, imu/raw_data·imu/mag_raw 발행 |
| element3 | kcf_mecanum_ekf | wheel_odometry·imu/raw_data 구독, ekf/odometry 발행 |

| 접두사 뒤 Topic 경로 | 메시지 형식 | 깊이 |
|---|---|---|
| /cmd_vel | mecanum::CmdVel | 1 |
| /wheel_state | mecanum::WheelState | 1 |
| /wheel_odometry | mecanum::WheelOdometry | 1 |
| /imu/raw_data | mecanum::imu::ImuSample | 16 |
| /imu/mag_raw | mecanum::imu::MagSample | 16 |
| /ekf/odometry | mecanum::ekf::EstimateMessage | 1 |

EKF의 세 Topic 인자는 같은 접두사로 명시적으로 연결합니다. 기존 고정 크기 메시지와 descriptor를 그대로 사용합니다. Magnetometer는 별도 관찰용이며 EKF에 융합하지 않습니다. cmd_vel 발행자가 없어도 Motor는 초기화 가능하며 목표는 0입니다.

시작 제한은 7초로 설정합니다. Motor의 기존 2.5초 부팅 대기를 포함한 시험용 여유이며 실시간 보장이 아닙니다. 상태 응답 제한과 종료 제한은 각 2초입니다. 정상 종료는 Bringup에 SIGTERM 또는 SIGINT를 보내면 기존 감독자가 세 자식에 종료를 요청하고 회수합니다. 상태 로그 및 Tool의 Application/Element 목록으로 확인합니다. 감독자 정상 종료와 실제 모터 안전 정지는 서로 다른 책임입니다.

## 재시작 범위

현재 Bringup 공개 API는 개별 자식을 교체하는 기능을 제공하지 않습니다. 단일 Bringup에서 자식 종료는 Application 오류로 기록되며, 재실행은 기본적으로 전체 구성 단위입니다. 내부 `RequestReset()`도 오류 상태에서 전체 세대를 재시작하는 API이며 기존 명령행에 외부 개별 재시작 명령은 없습니다.

`--component motor|imu|ekf`는 **선택한 실행파일 하나를 새 Bringup 아래 실행**하는 구성입니다. 통합 테스트에서 세 개의 요소별 Bringup을 실행해 EKF만 또는 입력 하나만 종료·재실행합니다. 이때 다른 프로세스의 PID 유지와 EKF의 Topic 세대·세션을 확인합니다. 단일 감독자의 개별 자식 자동 재시작을 검증한 것으로 해석하지 않아야 합니다.

## 가상 UART 통합 검증

가상 Motor UART는 기존 가상 UART 테스트와 같은 `m`·`r`·`e`·`z` 요청 및 응답 형식을 사용합니다. 모든 수신 목표 RPM이 0인지 검사합니다. Encoder 누적값은 외부에서 바퀴가 움직이는 상황을 가정하여 합성하며 물리적 모터는 구동하지 않습니다. IMU는 기존 합성 프레임과 같은 0x55 헤더·0x51~0x54 종류·검사합의 11바이트 프레임을 제공합니다.

검증 프로그램은 실제 세 실행파일을 별도 프로세스로 실행하고, 기존 Tool의 `KcfBackend`·값 해석 코드를 직접 연결하여 `Refresh`, `GetElements`, `StartTopicEcho`, `ReadTopicEcho`를 호출합니다. 대체 descriptor 해석기를 만들지 않습니다. Tool 저장소에는 쓰지 않습니다.

아래 Tool 경로는 이 환경의 설치 위치이며 다른 환경에서는 실제 경로로 바꿉니다.

```sh
c++ -std=c++17 -O2 -pthread \
  -I /home/kssvm/workspace/kcf/kcf_tools/include -I kcf/include \
  application/mecanum/tests/integration/tool_probe.cpp \
  /home/kssvm/workspace/kcf/kcf_tools/src/backend/kcf_backend.cpp \
  /home/kssvm/workspace/kcf/kcf_tools/src/backend/value_codec.cpp \
  /tmp/kcf-mecanum-ekf-build/kcf/libkcf_core.a -o /tmp/mecanum-tool-probe
python3 application/mecanum/tests/integration/run_integration.py \
  --build-dir /tmp/kcf-mecanum-ekf-build --probe /tmp/mecanum-tool-probe \
  --output-dir /tmp/mecanum-integration-results
```

일반 Linux 프로세스 환경에서 실행해야 합니다. 이번 실행의 격리 환경에서는 기존 `ProcessRuntime::StartSupervision()`의 Unix 감독 소켓 검사에서 `-EPROTO`가 발생했고, 동일한 바이너리를 일반 프로세스 환경에서 실행하면 시작 장벽을 통과했습니다. Core를 우회하거나 수정하지 않았습니다.

테스트는 다음을 확인하며 각 단계의 Tool 상태·해석된 필드를 JSON으로, 감독 상태를 로그로 저장합니다.

- 단일 Bringup의 세 RUNNING 프로세스와 서로 다른 PID, Topic별 실제 발행자 PID, descriptor 읽기.
- EKF 유효 출력과 sequence 진행, IMU 입력 중단 후 무효화·입력 복구.
- 정상 종료와 모든 자식 회수, 강제 종료가 사용되지 않았는지 확인.
- 요소별 Bringup을 통한 EKF 단독 재시작, 입력별 재시작과 EKF 재연결.
- IMU SIGKILL의 감독 오류 및 EKF 무효 상태, 없는 UART의 예상 초기화 실패.

Tool 확인 범위는 **실제 백엔드와 descriptor 기반 데이터 읽기**입니다. 매 관찰 시 새 조회·Echo 세션을 열므로 재시작 이후 재조회는 검증하지만 기존 GUI의 장기 Echo 세션 자동 재연결이나 실제 화면 조작·렌더링은 검증하지 않습니다. 기존 Tool의 Refresh 이후 다시 선택·읽기 하는 절차를 사용해야 합니다. 그래프·이력 기능은 추가하지 않았습니다.

실제 UART 지연·단절, 보드 재부팅, 물리적 정지·watchdog, 장착 축·방향 기준, 실제 이동 정확도는 별도 실기 검증 사항입니다. 입력 공백 중 로봇이 이동하면 EKF가 복구되더라도 누락 위치가 복원되거나 위치 정확도가 보장되는 것은 아닙니다.
