# KCF Timer Application 입문 예제

`ProcessElement` / `ProcessRuntime`에서 KCF Timer를 사용하는 예제입니다.
기존 `examples/timer`는 low-level API·회귀 테스트로 유지합니다.

- `src/timer_main.cpp`: 500 ms Timer, atomic tick counter, Loop 출력, Stop.
- `src/bringup_main.cpp`: `timer_example → timer_element` 실행.

Callback은 atomic `tick_count` 증가만 수행합니다. 별도의 20 Hz Runtime Loop가
snapshot을 읽어 새 tick을 출력합니다. 부하에 따라 중간 출력이 생략될 수 있으며,
로그 수신 시각에는 Loop·출력 지연이 포함됩니다. Hard real-time 보장은 없습니다.
첫 callback은 Create 후 한 주기가 지난 뒤 발생합니다.

## 빌드 및 실행

저장소 루트에서:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --target kcf_timer_app_bringup -j4
./build/examples/timer_app/kcf_timer_element
```

```text
[Timer] mode=Standalone
[Timer] tick=1
[Timer] tick=2
```

Standalone 종료 후 Supervised로 실행하려면:

```sh
./build/examples/timer_app/kcf_timer_app_bringup
```

Bringup과 Element binary는 같은 디렉터리에 둡니다. Bringup target은 Element도 빌드합니다.
Ctrl+C 또는 SIGTERM으로 종료합니다. 재실행하면 새 process의 tick은 1부터 시작합니다.

## Shutdown 계약

Shutdown은 Timer::Stop을 호출합니다. Stop은 실행 중 callback이 반환할 때까지 기다려
worker를 join합니다. Stop 반환 뒤에는 tick이 증가하지 않습니다. Callback state보다
Timer가 먼저 파괴되도록 member 순서도 유지합니다.

예제 callback은 기다림이나 I/O를 수행하지 않습니다. Callback이 오래 걸릴 때의 Stop
검증은 기존 `kcf_timer_test`의 in-flight callback join 테스트를 사용합니다.

Timer 자체의 Tool endpoint는 추가하지 않습니다. 기존 `kcf_tools --backend kcf`에서
Refresh하면 `timer_example → timer_element` Runtime/Application을 발견할 수 있습니다.
