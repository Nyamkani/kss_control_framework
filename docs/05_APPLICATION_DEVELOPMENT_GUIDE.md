# 05. Application Development Guide

## 예제 선택

| 종류 | 위치 | 목적 |
| --- | --- | --- |
| Low-level | [topic](../examples/topic/), [parameter](../examples/parameter/), [service](../examples/service/), [timer](../examples/timer/), [action](../examples/action/) | 개별 API/회귀 이해 |
| Application | [pubsub](../examples/pubsub/README.md) | ProcessElement/Runtime/Supervisor 첫 예제 |
| Application | [parameter_app](../examples/parameter_app/README.md) | Owner와 Tool 편집 |
| Application | [service_app](../examples/service_app/README.md) | 명시적 port와 Service Call |
| Application | [timer_app](../examples/timer_app/README.md) | local callback와 Stop |
| Application | [action_app](../examples/action_app/README.md) | Success/Cancel와 idle/RUNNING |

[USER REQUIREMENT / DECISION] 기존 low-level 예제를 보존하고 Application 예제를 별도로 둡니다.

## 권장 구성

다음은 Application 작성 시의 권장안이며 현재 자동 생성 도구나 필수 directory convention이 아닙니다.

```text
application/
├─ common/
├─ element_a/
├─ element_b/
└─ bringup/
```

각 Element: `main → argument/config → DetectLaunchExecutionMode → Element construct → ProcessRuntime → Run`.
Setup은 자원 생성과 초기화, Loop는 정상 cycle/FSM/실제 작업, Shutdown은 부분 초기화도 고려한 정리입니다.
Setup 진입 뒤 실패해도 Shutdown이 호출되므로 owned/open 플래그와 API 실패 cleanup 계약을 확인합니다.
Loop nonzero는 fatal입니다. 계속 운전할 수 있는 transient 오류만 Element 내부에서 복구하고 0을 반환합니다.

Standalone은 executable 직접 실행입니다. Supervised는 Bringup의 ElementSpec이 같은 executable/arguments를 spawn합니다.
Mode에 따라 executable을 복제하지 않습니다. 현재 app Bringup은 `/proc/self/exe`로 sibling binary 위치를 찾습니다.
Setup 성공과 Application RUNNING barrier 완료는 구분합니다.

## 기능 사용의 시작점

- Topic: [message descriptor](../examples/pubsub/include/pubsub/message.hpp),
  [Publisher](../examples/pubsub/src/publisher_main.cpp), [Subscriber](../examples/pubsub/src/subscriber_main.cpp).
  callback snapshot/generation을 lock 안에서 갱신하고 Loop의 I/O는 lock 밖에서 수행합니다.
- Parameter: [Owner](../examples/parameter_app/src/owner_main.cpp)의 Parameter::Create(initial, watcher).
  watcher는 알림만, 실제 current value는 Get으로 읽습니다. Close 후 Owner Unlink.
- Service: [Server](../examples/service_app/src/server_main.cpp)의 `ServiceServer::Create(PORT)`와 Register/Start.
  port를 Application이 정합니다. 이름만 지정하면 자동 transport endpoint가 생긴다고 가정하지 않습니다.
- Timer: [Element](../examples/timer_app/src/timer_main.cpp)의 Create(2.0, callback), Shutdown Stop.
  2 Hz는 500 ms입니다. callback이 캡처한 state보다 Timer가 먼저 파괴돼야 합니다.
- Action: [Server](../examples/action_app/src/server_main.cpp)의 progression Loop,
  [Client](../examples/action_app/src/client_main.cpp)의 결과 확인·Cancel. Server Stop 이후 Action Unlink/Close.
  두 scenario 후 client가 종료하지 않는 이유는 정상 Supervisor lifecycle을 유지하기 위해서입니다.

## 개발·검증 순서

1. CLI/port/logical name과 payload 소유권을 명시합니다. 같은 이름을 여러 app이 쓸 때 자동 namespace 분리를 기대하지 않습니다.
2. 작은 fixed-size payload를 정의합니다. Tool dynamic access가 필요하면 stable descriptor를 추가합니다.
3. Standalone에서 정상·오류·종료를 확인합니다. callback state lifetime과 partial Setup cleanup을 확인합니다.
4. 같은 executable을 Bringup으로 실행하고 RUNNING barrier, failure, restart를 확인합니다.
5. Tool은 수동 Refresh 후 새 identity를 선택합니다. Action/Timer 전용 UI는 기대하지 않습니다.
6. 기존 회귀는 fixed-name fixture와 충돌하지 않는 환경에서 순차 실행합니다. [검증 목록](06_VERIFICATION_AND_LIMITATIONS.md) 참조.

[FUTURE OPTION] 외부 Application에 대한 설치/export/package workflow는 이 예제의 CMake 사용법에서
자동으로 보장되지 않습니다. 현재 build는 저장소 내 명시적 add_subdirectory 구조입니다.
