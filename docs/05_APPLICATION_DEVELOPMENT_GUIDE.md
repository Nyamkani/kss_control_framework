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

## Topic Queue 사용 — v5.1

Publisher가 양수 Depth를 선택합니다. 생략하면 1이며 기존 Snapshot 코드와 Callback 동작을 유지합니다.
순차 소비가 필요하면 pull-only Subscriber를 만들고 Loop에서 ReadNext를 호출합니다.
다음은 API 호출 흐름이며 실제 Element는 Setup/Shutdown에서 자원 소유·정리를 관리합니다.
`Message`는 Application이 정의한 fixed-size trivially-copyable Payload입니다.

```cpp
kcf::Publisher<Message> publisher;
kcf::Subscriber<Message> subscriber;

// Setup: publisher가 이미 존재하는 다른 process에서는 Subscriber만 생성합니다.
int result = publisher.Create("/example/queue", 8);
if (result != 0) { /* 생성 실패 처리 */ }
result = subscriber.Create("/example/queue"); // 기본 NEXT
// 보관된 데이터부터 시작하려면 위 호출 대신:
// subscriber.Create("/example/queue", kcf::TopicStartPosition::OLDEST);

Message value{};
kcf::TopicReadInfo info{};
result = subscriber.ReadNext(value, info);
if (result == 0) {
    // info.sequence: Transport Sequence, info.missed: 이번 읽기에서 확인한 누락 수
    // Payload Timestamp/Validity 및 측정 Sequence를 Application 정책으로 판단
} else if (result == -EAGAIN) {
    // 새 데이터 없음 또는 복사 경합: Cursor 유지, 이후 cycle에서 재시도
} else {
    // 종료/연결 등 오류 처리
}

// 최신값 관찰이 필요할 때: 순차 Cursor에는 영향 없음
result = subscriber.ReadLatest(value, info);
```

`NEXT`는 연결 시점까지 Publish된 값을 순차 처리에서 제외합니다. `OLDEST`는 그 시점에 남아 있는
가장 오래된 값부터 시작합니다. Overflow는 KEEP_LAST로 오래된 값을 덮어쓰며,
느린 Subscriber는 성공한 ReadNext의 `missed`를 보고 자신의 누락을 처리합니다.
다른 Subscriber의 속도·읽기는 자신의 Cursor를 변경하지 않습니다. ReadLatest의 `missed`는 0이며
순차 소비 누락을 확인하는 API가 아닙니다. 기존 Callback과 Tool Echo도 최신값 방식으로 중간 값을 건너뛸 수 있습니다.

Publish가 `-EAGAIN`이면 안전한 Slot이 없어 Commit되지 않은 것이므로 전송 성공으로 세지 않습니다.
재시도/포기 정책은 Application이 결정합니다. 복사 경합으로 ReadNext가 실패해도 Cursor나 누락 집계를 이동시키지 않습니다.
Transport Sequence는 64-bit이며 성공한 Commit만 셉니다. Payload의 측정 Sequence·Timestamp·`valid`는 별도의 Application 계약입니다.
KCF는 `valid=false`도 그대로 전달하므로 Subscriber가 사용 가능 여부를 판단해야 합니다.

단일 Publisher/쓰기 thread를 유지하고 같은 Subscriber의 ReadNext 호출은 직렬화합니다.
Callback은 별도 worker이므로 Application state 공유에는 기존 동기화가 필요합니다.
Shutdown에서는 로컬 읽기를 끝내고 Subscriber Close로 Stop/join한 뒤 소유 자원을 정리합니다.
Dead-reader pin은 자동 회수되지 않습니다. 재생성된 SHM에는 Typed Subscriber Close/Create 또는 SharedChannel Close/Open으로
명시적으로 재연결해야 하며 시작 정책도 다시 적용됩니다. DynamicTopicReader의 `-ESTALE`도 Close/Open으로 처리합니다.

Topic Format 4는 이전 Topic Format 3과 바이너리 호환되지 않습니다. Publisher/Subscriber 및 Tool을
호환 Core로 함께 재빌드하고 이전 SHM은 소유자 lifecycle에 따라 명시적으로 정리합니다. 자동 migration은 없습니다.
Parameter Format 3은 유지합니다. [전체 계약](03_IPC_AND_FEATURES.md#topic)과
[보고된 Queue 검증](06_VERIFICATION_AND_LIMITATIONS.md#topic-queue-verification)을 참조합니다.

## 개발·검증 순서

1. CLI/port/logical name과 payload 소유권을 명시합니다. 같은 이름을 여러 app이 쓸 때 자동 namespace 분리를 기대하지 않습니다.
2. 작은 fixed-size payload를 정의합니다. Tool dynamic access가 필요하면 stable descriptor를 추가합니다.
3. Standalone에서 정상·오류·종료를 확인합니다. callback state lifetime과 partial Setup cleanup을 확인합니다.
4. 같은 executable을 Bringup으로 실행하고 RUNNING barrier, failure, restart를 확인합니다.
5. Tool은 수동 Refresh 후 새 identity를 선택합니다. Action/Timer 전용 UI는 기대하지 않습니다.
6. 기존 회귀는 fixed-name fixture와 충돌하지 않는 환경에서 순차 실행합니다. [검증 목록](06_VERIFICATION_AND_LIMITATIONS.md) 참조.

[FUTURE OPTION] 외부 Application에 대한 설치/export/package workflow는 이 예제의 CMake 사용법에서
자동으로 보장되지 않습니다. 현재 build는 저장소 내 명시적 add_subdirectory 구조입니다.

<a id="low-level-examples"></a>

## 기존 low-level 예제 실행

통합 전 README에서 현재도 유효한 실행법을 보존합니다. 명령은 저장소 루트와 Debug 기본 build 경로 기준입니다.
서버/Owner/Publisher를 먼저 실행하고 Client/Subscriber는 별도 터미널에서 실행합니다.
상시 실행 process는 Ctrl+C로 종료합니다. 이 문서 정리 중 예제를 재실행하지 않았습니다.

| 기능 | 실행 명령 | 인자·동작 |
| --- | --- | --- |
| Dummy | `./build/examples/dummy/kcf_dummy` | Standalone lifecycle, 약 2 Hz Loop |
| Topic Publisher | `./build/examples/topic/kcf_topic_pub 1000` | 발행 Hz; 기본 depth=1 |
| Topic Subscriber | `./build/examples/topic/kcf_topic_sub 0 1000` | Callback 지연(ms), 출력 간격; `350 1`은 느린 Callback 관찰 |
| Timer | `./build/examples/timer/kcf_timer_test` | 2/100 Hz, Stop, overrun, Topic 발행 후 자동 종료 |
| Service Server | `./build/examples/service/kcf_service_server 22000` | UDP port, Add/SetValue |
| Service Client | `./build/examples/service/kcf_service_client 22000 1000` | port, 연속 호출 수 |
| Parameter Owner | `./build/examples/parameter/kcf_parameter_owner` | 현재 설정 제공 |
| Parameter Client | `./build/examples/parameter/kcf_parameter_client` | 설정 읽기·변경; `watch` 인자로 변경 감시 |
| Action Server | `./build/examples/action/kcf_action_server 22010` | UDP port |
| Action Client | `./build/examples/action/kcf_action_client 22010 20` | port, target_count; 뒤에 callback_delay_ms, cancel_after_ms 선택 지정 |

Action의 느린 Feedback은 Client 인자 `22010 20 350`, 취소는 `22010 100 0 350`으로 관찰합니다.
Goal/Cancel/GetResult ID는 100/101/102, status Topic은 `/kcf_test_count_action/status`입니다.
최종 결과는 GetResult로 확인하며 모든 중간 Feedback 수신을 보장하지 않습니다.
통신 통합은 [integration](../examples/integration/), 오류·Reset은 [supervisor](../examples/supervisor/),
SHM 복구는 [recovery](../examples/recovery/)를 참고합니다. 검증 명령과 과거 결과는 [06](06_VERIFICATION_AND_LIMITATIONS.md)에 있습니다.
