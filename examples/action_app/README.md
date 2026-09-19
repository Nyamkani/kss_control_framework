# KCF Action Application 입문 예제

Action은 Goal을 받고 진행 상태를 Feedback으로 알린 뒤 Result로 완료합니다.
Cancel은 취소 요청이며 Server Loop가 Canceled를 호출해야 완료됩니다.
기존 `examples/action`는 low-level API·회귀 예제로 유지합니다.

## 구조

- `include/action_app/count_action.hpp`: 독립 Goal/Feedback/Result POD, endpoint IDs, port 파싱.
- `src/server_main.cpp`: ServiceServer + ActionServer, 100 ms Loop에서 진행.
- `src/client_main.cpp`: ActionClient, Success → Cancel 자동 실행 후 idle/RUNNING 유지.
- `src/bringup_main.cpp`: `action_example → action_server / action_client` 실행.

Server는 1~10000의 target만 받습니다. Callback은 target 저장 또는 최소 취소 처리만
수행하며, count 증가·feedback·완료 출력은 Loop에서 처리합니다.
Client callback은 mutex 아래 snapshot만 복사합니다. 출력과 Service 호출은 Loop에서 합니다.

첫 goal은 target 10이며 `SUCCEEDED, final=10, code=0`을 확인합니다.
두 번째는 target 50이며 약 500 ms 후 취소하여 `CANCELED, code=-ECANCELED`를 확인합니다.
단일 active goal 기준이며 queue/preemption은 추가하지 않습니다. Feedback은 latest-value
snapshot이므로 모든 중간 상태·sample 수신을 보장하지 않습니다.
Goal ID는 임의 seed 기반이므로 1부터 시작한다고 가정하지 않습니다.

## 빌드

저장소 루트에서:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --target kcf_action_app_bringup -j4
```

기존 target과 구분하기 위해 target은 `kcf_action_app_server/client`,
실행 파일은 `kcf_action_server/client`입니다. Bringup target이 둘 다 빌드합니다.

## Standalone

별도 터미널에서 Server를 먼저 실행합니다:

```sh
./build/examples/action_app/kcf_action_server 22120
```

```sh
./build/examples/action_app/kcf_action_client 22120
```

두 scenario 완료 후에도 Client는 RUNNING을 유지합니다. Ctrl+C 또는 SIGTERM으로
Client와 Server를 종료합니다. Client 연결은 최대 약 5초 재시도하며, 실행 중 통신 오류는
현재 API의 bounded timeout을 거쳐 Runtime 오류로 반환합니다. Goal 진행 제한은 10초입니다.
완료 후 idle 상태에서는 Server 생존을 별도로 polling하지 않습니다.

## Supervised

Standalone 종료 후:

```sh
./build/examples/action_app/kcf_action_app_bringup 22120
```

세 binary는 같은 디렉터리에 둡니다. Bringup은 같은 port를 두 Element에 전달합니다.
Port 생략 시 22120이며, **동시에 여러 Action Server를 실행할 때는 서로 다른 port를
지정해야 합니다.** 내부 status 이름도 `/example/action/count/<port>`로 구분합니다.
Core 자동 port 할당은 사용하지 않습니다.

Shutdown은 Server의 Service worker를 먼저 Stop하고 Action을 Unlink/Close합니다.
Client는 callback worker를 join하고 통신 자원을 닫습니다. Server 비정상 종료 시
Supervisor ERROR는 기존 contract를 따릅니다. 재실행은 새 process에서 scenario를 반복합니다.

## Tool

기존 `kcf_tools --backend kcf`의 Refresh로 `action_example`과 두 Runtime만 확인합니다.
Action 전용 endpoint/UI는 없으며 Goal/Feedback/Result를 Tool에서 조작하지 않습니다.
Action 내부 Service/status Topic을 넘어서는 별도 통신 기능은 추가하지 않습니다.
