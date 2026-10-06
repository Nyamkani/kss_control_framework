# KCF Pub/Sub 입문 예제

ROS talker/listener처럼 Publisher와 Subscriber를 실행하면서 KCF의
ProcessElement → ProcessRuntime → Supervisor 구조를 익히는 예제입니다.
`examples/topic`의 low-level IPC 예제와 별도로 Application lifecycle과
callback/Loop 동시성 계약을 보여줍니다.

## 구조

- `include/pubsub/message.hpp`: flat POD와 R3 stable TypeDescriptor.
- `src/publisher_main.cpp`: 1 Hz 발행, monotonic microsecond timestamp.
- `src/subscriber_main.cpp`: callback에서 mutex 아래 snapshot/generation 갱신,
  10 Hz Loop에서 local copy 후 새 generation만 출력.
- `src/bringup_main.cpp`: `pubsub_example`의 `publisher`·`subscriber` 실행.

Topic은 `/example/chatter`, canonical type name은 `kcf.example.PubSubMessage`입니다.
이 예제의 계약은 version 1이며 R3 descriptor의 `protocol_version`은 1입니다.
R3에는 별도의 Application schema version 필드가 없으므로, 이후 비호환 schema를
만들 때는 canonical name도 새로 정해 stable type ID 충돌을 피해야 합니다.
필드는 `sequence`, `timestamp_us`, `count`, `value`, `active`입니다.

Publisher가 Topic owner이며 Shutdown에서 Close/Unlink합니다. Subscriber는 최대
10초 동안 `-ENOENT`/`-EAGAIN`에서만 연결을 재시도합니다. Callback은 출력·sleep·I/O 없이
복사만 수행하고, Runtime Loop가 출력합니다. Latest-value Topic이므로 모든 sample의
순차 전달을 보장하지 않습니다. ExecutionMode는 Run 전에 감지합니다.

## 빌드

R3/introspection 기능이 있는 Framework 저장소 루트에서:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --target kcf_pubsub_bringup -j4
```

이 target은 두 child executable도 함께 빌드합니다.

## Standalone — 별도 터미널 두 개

```sh
./build/examples/pubsub/kcf_pubsub_publisher
```

```sh
./build/examples/pubsub/kcf_pubsub_subscriber
```

약 1초마다 다음처럼 같은 sample을 출력합니다.

```text
[Publisher] seq=3 count=3 value=1.5
[Subscriber] seq=3 count=3 value=1.5
```

Ctrl+C로 종료합니다. Subscriber를 먼저 종료한 뒤 Publisher를 종료하면 됩니다.

## Bringup

Standalone 두 프로세스를 종료한 뒤 실행합니다. 같은 Topic에 publisher 둘을
동시에 실행하지 않습니다.

```sh
./build/examples/pubsub/kcf_pubsub_bringup
```

같은 디렉터리의 두 실행 파일을 찾으므로 세 binary를 함께 둡니다.
Supervisor가 전체 Runtime RUNNING을 확인하며 Ctrl+C/SIGTERM으로 child들을 종료·회수합니다.
별도의 supervised 전용 executable은 없습니다.

## kcf_tools v0.1

동일한 introspection Framework revision으로 빌드한 기존 Tool에서:

```sh
/path/to/kcf_tools/build-kt3/kcf_tool --backend kcf
```

1. Application 실행 후 **Refresh**를 누릅니다.
2. Standalone 실행이면 **Standalone** 아래 두 executable이 표시됩니다.
3. Bringup 실행이면 **pubsub_example → publisher / subscriber**를 확인합니다.
4. 각 Element의 `/example/chatter`에서 **PUBLISHER / SUBSCRIBER** 역할을 확인합니다.
5. Publisher endpoint를 선택하고 **Start Echo**를 누릅니다.
6. 다섯 field가 자동 표시되고 약 1초마다 sequence/count/value/timestamp가 바뀌며
   active가 true인지 확인합니다. **Stop Echo**로 관찰을 종료합니다.

Tool은 예제의 C++ 메시지 헤더를 포함하지 않고 R3 descriptor로 field를 해석합니다.
