# KCF Parameter Application 입문 예제

`ProcessElement` / `ProcessRuntime` 기반 설정값 Owner 예제입니다.
기존 `examples/parameter`는 low-level IPC 예제로 유지합니다.

## 구조와 계약

- `include/parameter_app/example_config.hpp`: trivially-copyable `ExampleConfig`와 R3 descriptor.
- `src/owner_main.cpp`: 초기값 생성, 10 Hz Get, 값 변경 출력, 종료 정리.
- `src/bringup_main.cpp`: `parameter_example → parameter_owner` 실행.

Parameter 이름은 `/example/config`, canonical type name은
`kcf.example.ExampleConfig`입니다. 계약 version은 1이며 R3 `protocol_version`도 1입니다.
R3에는 별도 Application schema version 필드가 없으므로 비호환 schema에는 새 canonical
name을 사용해야 합니다.

`Parameter<ExampleConfig>`는 내부 `SharedParameter<ExampleConfig>` Owner와 watcher,
introspection endpoint 등록을 제공합니다. Callback은 atomic 알림 플래그만 설정합니다.
Loop는 callback snapshot 대신 Get으로 실제 현재 값을 읽고 field별로 비교합니다.
Watcher 알림은 합쳐질 수 있으며 모든 중간 변경의 전달을 보장하지 않습니다.
Shutdown은 watcher를 종료·join하고 Close/Unlink합니다.

## 빌드

저장소 루트에서:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --target kcf_parameter_app_bringup -j4
```

기존 target과 충돌하지 않도록 Owner target은 `kcf_parameter_app_owner`,
출력 파일명은 `kcf_parameter_owner`입니다. Bringup target은 Owner도 빌드합니다.

## 실행

Standalone:

```sh
./build/examples/parameter_app/kcf_parameter_owner
```

또는 Standalone 종료 후 Supervised 실행:

```sh
./build/examples/parameter_app/kcf_parameter_app_bringup
```

두 모드를 동시에 실행하지 않습니다. Bringup과 Owner binary는 같은 디렉터리에 둡니다.
Ctrl+C 또는 SIGTERM으로 종료합니다.

## kcf_tools v0.1에서 확인

```sh
/path/to/kcf_tools/build-kt3/kcf_tool --backend kcf
```

1. **Refresh** 후 Standalone의 `kcf_parameter_owner`, 또는
   `parameter_example → parameter_owner`를 확인합니다.
2. **Parameters**에서 `/example/config`의 `OWNER` endpoint를 선택합니다.
3. 초기값 `enabled=true`, `count=10`, `gain=1`, `limits=[-1, 1]`을 확인합니다.
4. `false`, `20`, `2.5`, `[-2, 2]`로 편집하고 **Apply**합니다.
   Tool의 재조회 값과 Owner 출력을 확인합니다:

```text
[Parameter] changed
[Parameter] enabled=false count=20 gain=2.5 limits=[-2, 2]
[Parameter] watcher notification
```

Watcher 출력 순서는 고정되지 않습니다.

5. 편집만 하고 **Revert**하면 실제 값은 변경되지 않습니다.
6. `count=abc`, `gain=nan/inf`, `limits[0]=1e999`는 Apply 시 validation에서
   거부되며 저장값은 유지됩니다.
7. Owner 종료 후 선택을 유지한 채 **Refresh Value**하면 stale 오류가 표시됩니다.
   **Refresh**로 endpoint를 제거하고, 재실행 후 새 endpoint를 선택합니다.

별도 Application 값 변경 명령 경로는 제공하지 않습니다. 외부 변경 후 Refresh Value
시나리오는 생략하며, Apply 재조회와 Refresh Value의 현재값 조회를 확인합니다.
Tool은 예제 메시지 헤더 없이 R3 descriptor로 scalar와 배열 field를 해석합니다.
