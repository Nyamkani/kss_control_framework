# v5.5 갱신 및 Keyboard 검증 결과

검증일: 2026-10-23. 작업 위치는 `/home/kssvm/workspace/kcf/mecanum/kss_control_framework`, 브랜치는 `mecanum`입니다. 새 브랜치·커밋·푸시 없이 시작 HEAD `70267a7`을 유지했습니다.

## 반영·보존 확인

- `f184787`의 실제 변경 경로 95개를 반영했습니다. 전체 목록은 `UPSTREAM_FILES.txt`에 있습니다.
- Core·Bringup 99개 파일이 해당 객체와 바이트 단위로 일치합니다. 루트 CMake 외 upstream 변경 94개 경로도 일치합니다.
- 기존 Mecanum 파일 62개 중 변경은 통합 테스트 한 곳(5줄 추가·1줄 대체)뿐입니다. 장치·필터·펌웨어·메시지·기존 Bringup 실행 구성은 그대로입니다.
- 테스트 변경 이유: v5.5의 초기 자식 실패 시 ERROR 유지 계약에 맞춰, 즉시 종료를 기다리는 대신 ERROR를 관찰하고 종료를 요청합니다. Core나 Supervisor를 기존 동작으로 되돌리지 않았습니다.
- 텍스트 병합 충돌은 없었습니다. v5.5 upstream의 `Changelog.md` 84행 끝 공백은 원본 동일성을 위해 유지했으며, Application·루트 CMake 범위의 공백 검사는 통과했습니다.

## 빌드·회귀

| 항목 | 결과 |
|---|---|
| Framework 전체 Debug 빌드, 모든 기본 대상 포함 | PASS |
| Motor 회귀 | PASS |
| WT901C IMU 회귀 | PASS |
| EKF 회귀 | PASS |
| EKF 독립 기준 궤적 20개 조합 | PASS |
| v5.5 Runtime 수명주기 | PASS |
| v5.5 Supervisor 실패·종료 정리 계약 | PASS |
| 기존 가상 UART 통합, 초기 오류 상태 확인 포함 14개 항목 | PASS |
| 기존 비영점 직진·횡이동·회전 통합 | PASS |
| Keyboard W/S/A/D/Q/E → CmdVel → Motor UART | PASS |
| Space·입력 중단·Esc | PASS |
| SIGINT/SIGTERM 마지막 영점·터미널 복원 | PASS |
| SIGKILL 이후 기존 Motor 만료 정지 | PASS |
| Keyboard와 Supervisor 독립성·관리 실행 거절 | PASS |
| 동일 Topic 중복 Publisher 거절 | PASS |
| Publisher 재생성 후 기존 Motor 미재연결 제한 재현 | PASS: 제한 확인 |

최초 관찰 프로그램 링크는 현재 설치된 Tool의 `network_backend.cpp` 의존성이 빠져 실패했으며, 수정 없이 해당 파일을 링크하여 해결했습니다. 현재 Tool의 로컬 관찰 경로만 사용합니다. 초기 기존 통합 시험의 즉시 종료 가정은 v5.5에서 시간 초과됐으며, 위 계약 차이를 반영한 최종 시험은 통과했습니다.

기존 IMU SDK의 부호 비교 및 switch fall-through 경고 2개는 그대로입니다. 새 Keyboard 컴파일 경고는 없었습니다. 결과는 Debug/PTY 소프트웨어 시험이며 하드 실시간이나 물리 정지 성능을 보장하지 않습니다.

## Keyboard 실측

시험 선속도 0.12m/s, 각속도 0.4rad/s, 입력 만료 150밀리초입니다. RPM 순서는 Arduino 1,2,3,4입니다.

| 키 | 실제 CmdVel (vx,vy,wz) | UART 수신 RPM | Space 입력→0 RPM 수신(ms) |
|---|---|---|---|
| W | [0.12, 0.0, 0.0] | [-27.949162, -27.949162, -27.949162, -27.949162] | 9.066 |
| S | [-0.12, 0.0, 0.0] | [27.949162, 27.949162, 27.949162, 27.949162] | 6.486 |
| A | [0.0, 0.12, 0.0] | [27.949162, -27.949162, 27.949162, -27.949162] | 20.636 |
| D | [0.0, -0.12, 0.0] | [-27.949162, 27.949162, -27.949162, 27.949162] | 6.459 |
| Q | [0.0, 0.0, 0.4] | [18.399864, 18.399864, -18.399864, -18.399864] | 5.481 |
| E | [0.0, 0.0, -0.4] | [-18.399864, -18.399864, 18.399864, 18.399864] | 24.063 |

- 마지막 키 쓰기→입력 중단 영점 UART 수신: **174.429ms**.
- SIGINT→0 RPM 수신: 20.887ms, 최종 영점 메시지와 정상 종료 확인.
- SIGTERM→0 RPM 수신: 10.244ms, 최종 영점 메시지와 정상 종료 확인.
- SIGKILL 시험에서 마지막 관측 비영점 명령 생성 시각→0 RPM 수신: **254.606ms**. 기존 250ms 만료 기준은 유지했습니다.

Esc 종료에서 마지막 영점 메시지, UART 영점 요청, Close/Unlink, 터미널 복원을 확인했습니다. Keyboard는 런타임 관찰상 STANDALONE이며, Supervisor 종료 후에도 살아 있고 Keyboard 종료 후 Supervisor도 유지됩니다. Supervisor에 넣어 시작하면 Keyboard가 거절하고 v5.5 Supervisor가 ERROR로 남는 것을 확인했습니다.

Publisher를 재생성한 뒤 새 Topic의 비영점 CmdVel을 실제로 읽었지만 기존 Motor에는 비영점 RPM이 전달되지 않았습니다. 기존 Subscriber의 재연결 제한이며 이번에는 수정하지 않았습니다.

## 실장치 적용 전 남은 사항

키 반복 초기 지연과 입력 유효기간의 관계를 확인해야 합니다. 터미널은 키 해제 사건을 제공하지 않으며 입력 중단 정지는 설정된 유효기간 이후입니다. 속도 인자는 실제 장치의 안전 한도를 보증하지 않습니다.

마지막 영점 발행 성공은 수신·물리 정지 보장이 아닙니다. SIGKILL/정전은 영점 전송과 터미널 복원이 불가능하며 Motor timeout과 별도 MCU 정지 계약에 의존합니다. Keyboard 재실행 후에는 Motor의 명령 구독 연결도 다시 수립해야 합니다.

실제 UART 부하, MCU watchdog, 모터 정지, 센서 방향·슬립·EKF 정확도는 실기 검증으로 남깁니다. 실제 장치 실행·업로드·서비스 등록은 하지 않았습니다.

전체 소프트웨어 검증 요약과 키 관측값은 `tests/validation_results.json`에 보존했습니다. 상세 임시 로그는 `/tmp/v55-*.log`, `/tmp/v55-keyboard-measured/`, `/tmp/v55-integration-pass/`, `/tmp/v55-motion-final/`에 있습니다. 임시 경로는 환경 정리 시 사라질 수 있습니다.
