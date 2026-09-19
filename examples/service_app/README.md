# KCF Service Application 입문 예제

`ProcessElement` / `ProcessRuntime` 기반 덧셈 Service입니다.
기존 `examples/service`는 low-level IPC 예제로 유지합니다.

## 구조와 계약

- `include/service_app/add_service.hpp`: flat POD request/response와 R3 descriptor.
- `src/server_main.cpp`: 등록, 기존 Service worker callback, 종료 정리.
- `src/bringup_main.cpp`: `service_example → service_server` 실행.

Service `/example/add`는 loopback UDP port **22210**, service ID **1**을 사용합니다.
Canonical type은 `kcf.example.AddRequest`, `kcf.example.AddResponse`입니다.
계약 version과 R3 `protocol_version`은 1입니다. 별도 schema version 필드가 없으므로
비호환 schema에는 새 canonical name을 사용합니다.

Request는 int32 `a`, `b`, response는 int32 `result`, bool `accepted`입니다.
합이 int32 범위이면 합계와 `true`, 합산 overflow이면 `0`과 `false`를 반환합니다.
기본 callback은 즉시 계산·출력합니다. Loop는 10 Hz Runtime cycle만 유지하며,
Shutdown의 Stop이 worker를 join하고 socket과 Service 등록을 정리합니다.

## 빌드와 실행

저장소 루트에서:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --target kcf_service_app_bringup -j4
./build/examples/service_app/kcf_service_server
```

기존 target과 충돌하지 않도록 새 server target은 `kcf_service_app_server`,
출력명은 `kcf_service_server`입니다. Bringup target은 server도 빌드합니다.
Standalone 종료 후 Supervised로 실행하려면:

```sh
./build/examples/service_app/kcf_service_app_bringup
```

두 binary는 같은 디렉터리에 둡니다. 두 모드를 동시에 실행하지 않으며 port 22210은
비어 있어야 합니다. Ctrl+C 또는 SIGTERM으로 정상 종료합니다.

## kcf_tools v0.1

```sh
/path/to/kcf_tools/build-kt3/kcf_tool --backend kcf
```

1. **Refresh**에서 Standalone의 `kcf_service_server`, 또는
   `service_example → service_server`를 확인합니다.
2. **Services**의 `/example/add`를 선택합니다. Descriptor로 `a`, `b` 입력란이 생성됩니다.
3. `10`, `25` 입력 후 **Call**하면 `result=35`, `accepted=true`입니다.
   Server도 같은 request/response를 출력합니다.
4. `-5 + 2 = -3`, `0 + 0 = 0`도 확인합니다.
5. `abc`와 int32 범위 밖의 입력은 handler 호출 전에 validation에서 거부됩니다.

Call 진행 중에는 중복 Call이 차단됩니다. 기존 KCF 내부 retry는 유지되며 Tool이
추가 retry를 수행하지 않습니다. Timeout은 응답을 받지 못했다는 의미이며,
**server callback이 실행되지 않았다는 보장이 아닙니다.**

선택된 server 종료 후 Call은 stale/실패 오류가 됩니다. Refresh 후 endpoint가 제거되며,
재실행 후 새 runtime identity의 Service를 선택해야 합니다. Registration ID는 runtime
범위 값이므로 재시작 시 같은 숫자가 나올 수 있습니다. PID/start_ticks와 함께 식별합니다.

예제에는 지연 옵션을 추가하지 않습니다. 실제 timeout 검증에서는 standalone test server만
SIGSTOP으로 정지한 뒤 Call했고, GUI 응답성과 timeout 문구를 확인했습니다.
SIGCONT 후 지연된 handler 실행도 확인했습니다. 이 과정은 Tool/Framework 코드 수정 없이 수행합니다.
