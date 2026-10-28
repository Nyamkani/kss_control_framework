# Keyboard Standalone — 현재 Framework v5.6.0

현재 마이그레이션·회귀 결과는 [v5.6 검증](../V56_MIGRATION.md)을 참고하세요. 아래 Framework v5.5 반영 방법과 당시 테스트 기록은 Historical 자료입니다. Keyboard 동작 계약은 유지합니다.

## 실행

`kcf_mecanum_keyboard`는 터미널에서 직접 실행하는 Linux C++17 Standalone입니다. Bringup의 관리 목록에 넣지 않습니다. 감독 실행 환경을 감지하면 시작을 거절합니다. ROS나 새 설정 서비스는 사용하지 않습니다.

```sh
cmake -S . -B /tmp/mecanum-v56-debug -DCMAKE_BUILD_TYPE=Debug \
  -DMECANUM_BUILD_TESTS=ON -DMECANUM_BUILD_IMU_TESTS=ON -DMECANUM_BUILD_EKF_TESTS=ON
cmake --build /tmp/mecanum-v56-debug -j4
/tmp/mecanum-v56-debug/application/mecanum/keyboard/kcf_mecanum_keyboard \
  --linear-speed 0.15 --angular-speed 0.5 --input-timeout-ms 150 --topic-prefix /mecanum
```

실제 모터에 연결한 실행은 별도 실기 검증 단계입니다. 위 명령은 자동 실행·서비스 등록을 만들지 않습니다. UART와 Motor·IMU·EKF의 실행 설정은 기존 `../bringup/README.md`를 따릅니다.

| 입력 | 명령 |
|---|---|
| W / S | +vx / -vx, 전진 / 후진 |
| A / D | +vy / -vy, 좌 / 우 횡이동 |
| Q / E | +wz / -wz, 좌 / 우 회전 |
| Space | 영점 |
| Esc | 영점 발행 시도 후 종료 |

대소문자를 모두 받습니다. 각 이동 키는 이전 축 명령을 대체하며 대각선 조합을 누적하지 않습니다. 다른 키는 이동 유효기간을 연장하지 않습니다. 방향키의 Escape 시작 문자도 종료로 취급합니다.

선속도는 m/s, 각속도는 rad/s이며 유한한 양수만 받습니다. 이 설정은 실제 모터의 검증된 안전 속도 제한을 뜻하지 않습니다. 입력 유효기간은 20~250밀리초, 기본 150밀리초입니다. 실행 중 설정은 바꾸지 않습니다.

## 명령과 정지 계약

기존 `mecanum::CmdVel`과 KCF `Publisher`를 그대로 사용하여 `<topic-prefix>/cmd_vel`, 깊이 1에 발행합니다. 기본 경로는 `/mecanum/cmd_vel`입니다. 매 발행마다 sequence를 증가시키고 `CLOCK_MONOTONIC` 생성 시각을 새로 기록합니다. 목표 발행 주기는 50Hz이며 실시간 보장은 아닙니다.

이동 키 수신 후 기본 150밀리초가 지나면 영점으로 전환하여 계속 발행합니다. 터미널은 키를 놓는 사건을 제공하지 않으므로 OS의 키 반복에 의존합니다. 길게 눌렀을 때 최초 반복 지연이 유효기간보다 길면 잠깐 정지할 수 있습니다. 입력이 끊긴 상태에서 과거 비영점 명령을 무한히 새 시각으로 연장하지 않습니다.

Space, Esc, 입력 단절, SIGINT/SIGTERM 정상 처리 및 오류 종료의 Shutdown에서 영점 발행을 시도합니다. 발행 오류와 자원 정리 실패는 성공으로 숨기지 않고 비영 종료코드로 보고합니다. 정상 종료 시 Publisher Close/Unlink와 터미널 설정 복원을 수행합니다. 발행 성공은 Motor 수신이나 물리 정지 확인을 뜻하지 않습니다.

SIGKILL·크래시·정전에는 마지막 영점 및 터미널 복원을 보장하지 않습니다. 기존 Motor의 마지막 명령 생성 시각 기준 250밀리초 만료에 의존합니다. Motor 자체도 멈춘 경우는 MCU watchdog 등 별도의 책임이며 이번에 변경하지 않았습니다.

## Publisher 재생성과 독립성

동일 경로에 살아 있는 Publisher가 있으면 두 번째 Keyboard 시작을 거절합니다. 기존 Motor는 한 번 연결한 `cmd_vel`의 공유 메모리에 머물며 Keyboard 종료·Unlink 후 새 Publisher의 이름을 자동 재연결하지 않습니다. 재생성된 Topic에서 비영점 값이 보여도 기존 Motor에는 전달되지 않을 수 있습니다. 시험에서 이 제한을 재현했습니다. 기존 Motor의 재연결 로직은 이번에 바꾸지 않았습니다.

Keyboard를 새로 시작한 뒤 기존 Motor의 명령 연결도 다시 수립해야 합니다. 현 구성에서는 Motor를 포함한 적절한 Bringup 재시작이 필요할 수 있으며 자동 재시작이나 실장치 실행은 이 프로그램이 수행하지 않습니다. Keyboard 종료가 Supervisor를 종료시키지 않고, Supervisor 종료도 Keyboard를 관리 자식처럼 종료시키지 않습니다.

## Framework 반영 방법

작업 저장소는 `/home/kssvm/workspace/kcf/mecanum/kss_control_framework`, 브랜치는 `mecanum`입니다. 시작 HEAD는 `70267a7`이며 대상은 로컬 `origin/dev`가 가리키는 `f184787d7d68e8e1d3adbcd27d9ee09d170893a9`입니다. 네트워크 최신 버전을 추정한 것이 아니라 지정된 객체를 사용했습니다.

공통 기준 `8360b6e` 이후 dev 변경 95개 경로를 대상 객체에서 작업 트리에 반영했습니다. 별도 브랜치·병합 커밋·일반 커밋을 만들지 않았으며 HEAD는 유지됩니다. 루트 CMake는 대상 v5.5의 네트워크·Application composition 연결까지 보존하고 기존 Mecanum 3개 및 Keyboard 연결을 추가했습니다. Core·Bringup 99개 파일은 대상 객체와 바이트 단위로 일치합니다. 단순 버전 문자열 변경이 아닙니다.

파일 충돌은 없었습니다. 기존 Mecanum의 Framework 쪽 변경은 루트 CMake 연결뿐이어서 Application 소스를 덮어쓸 필요가 없었습니다. Motor·IMU·EKF·Arduino 원본 구현과 메시지·250밀리초 계약은 보존했습니다.

확인된 호환성 차이는 v5.5 Supervisor의 초기 자식 실패 처리입니다. 기존에는 즉시 종료를 기대했지만 v5.5는 ERROR 상태로 남아 종료/복구 요청을 기다립니다. `../tests/integration/run_integration.py`의 해당 검증 한 곳만 ERROR 확인 후 SIGTERM 및 오류 종료코드 확인으로 맞췄습니다. Supervisor 동작 자체는 지정된 v5.5 그대로입니다.

## 테스트 재현

가상 UART는 기존 `MotionUarts`를 가져와 재사용합니다. 키 입력용 PTY와 UART용 PTY는 서로 다르며 실제 장치는 사용하지 않습니다. 명령 관찰자는 기존 메시지를 읽기만 하며 종료 직후 마지막 영점도 확인할 수 있도록 동일 공유 메모리 연결을 유지합니다.

```sh
c++ -std=c++17 -g -pthread -I kcf/include -I application/mecanum/include \
  application/mecanum/keyboard/tests/cmd_watch.cpp /tmp/mecanum-v56-debug/kcf/libkcf_core.a -o /tmp/v55-cmd-watch
# 현재 설치된 Tool에는 network_backend.cpp도 링크해야 합니다. Tool 소스 변경은 없습니다.
c++ -std=c++17 -g -pthread -I /home/kssvm/workspace/kcf/kcf_tools/include -I kcf/include \
  application/mecanum/tests/integration/tool_probe.cpp \
  /home/kssvm/workspace/kcf/kcf_tools/src/backend/kcf_backend.cpp \
  /home/kssvm/workspace/kcf/kcf_tools/src/backend/value_codec.cpp \
  /home/kssvm/workspace/kcf/kcf_tools/src/backend/network_backend.cpp \
  /tmp/mecanum-v56-debug/kcf/libkcf_core.a -o /tmp/v55-tool-probe
PYTHONDONTWRITEBYTECODE=1 python3 application/mecanum/keyboard/tests/run_keyboard.py \
  --build-dir /tmp/mecanum-v56-debug --probe /tmp/v55-tool-probe --watch /tmp/v55-cmd-watch \
  --output-dir /tmp/v55-keyboard-results
```

기존 Motor·IMU·EKF 테스트, 가상 UART 통합 및 비영점 테스트에도 같은 Debug 빌드 경로를 전달합니다. Supervisor 소켓과 프로세스 메타데이터를 지원하는 일반 Linux 환경에서 실행합니다. 시험 결과와 실제 측정 시간은 `VALIDATION.md`를 참고하세요.
