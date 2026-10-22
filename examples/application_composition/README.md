# Application Composition example

기존 ProcessRuntime, Publisher/Subscriber, Bringup을 조합하는 Linux 예제입니다.

| 프로세스 | 주기 | 역할 / Topic 소유권 |
|---|---:|---|
| `kcf_composition_source` | 100Hz | sequence와 CLOCK_MONOTONIC timestamp_ns, value=sequence×0.5를 Publish. source Topic 소유 |
| `kcf_composition_processor` | 50Hz | source 최신 Snapshot을 읽어 value×2를 Publish. 원본 sequence/timestamp/source_value 보존. result Topic 소유 |
| `kcf_composition_observer` | 10Hz | result 최신 Snapshot 조회 및 Age/상태 출력. Topic 생성·Unlink 없음 |
| `kcf_composition_bringup` | 기존 Supervisor 정책 | Source/Processor만 ElementSpec으로 관리. ExecutionProfile 기본값 |

Observer는 외부 독립 프로세스입니다. Bringup은 Observer를 실행·감시·종료·회수하지 않습니다.
모든 Element는 기존 ProcessRuntime의 Signal/Shutdown 처리를 사용합니다.

## Build

저장소 루트에서:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --target kcf_composition_bringup kcf_composition_observer -j4
cd build/examples/application_composition
```

## Managed + External 실행

세션마다 고유한 INSTANCE를 정하고 모든 프로그램에 동일한 값을 전달합니다.
문자/숫자/`_`/`-` 1~40자를 허용합니다. 아래 예시는 `demo_01`입니다.

터미널 1:

```bash
./kcf_composition_bringup demo_01
```

`INITIALIZING -> RUNNING` 및 source/processor `state=2`를 확인한 뒤 터미널 2:

```bash
./kcf_composition_observer demo_01
```

예상 출력 형식(Sequence와 Age는 실행마다 다름):

```text
[Source] ... mode=Supervised hz=100
[Processor] ... mode=Supervised hz=50
[Observer] ... mode=Standalone hz=10
[Observer] seq=101 timestamp_ns=... value=101 age_ms=... status=NEW received=... repeated=... skipped=... duplicates=0 reversed=0
```

Observer에서 Ctrl+C를 눌러도 Supervisor와 Source/Processor는 계속 실행됩니다.
Observer를 다시 명시적으로 실행하거나, 실행 중인 Observer에서 `r`을 입력하면 현재 result Topic을 조회합니다.
Bringup에서 Ctrl+C를 누르면 관리 자식만 종료·회수합니다. 남은 Observer는 직접 종료하세요.

## 동일 PID에서 명시적 재연결

Observer 터미널에서 **`r` + Enter**를 입력합니다. stdin은 비차단으로 확인하며,
재연결은 기존 10Hz Loop에서 수행합니다. 별도 Thread나 제어 서버를 만들지 않습니다.
같은 입력 묶음에 여러 `r`이 있으면 한 번의 요청으로 처리합니다.

1. Bringup만 정상 종료하거나, Managed Source 장애 후 ERROR 상태의 Bringup을 종료합니다.
2. Observer는 이전 SHM 매핑을 읽으며 마지막 데이터의 Age가 증가하고 `STALE`로 표시됩니다.
3. 동일 INSTANCE로 Bringup을 다시 실행합니다. Observer는 자동으로 새 SHM에 연결하지 않습니다.
4. Observer에서 `r`을 입력합니다. PID는 유지한 채 기존 Subscriber를 Close하고,
   Snapshot·Transport Sequence·원본 Sequence/Timestamp 비교 기준과 수신 통계를 초기화한 뒤
   동일 Topic에 Create를 **한 번** 시도합니다.
5. 성공하면 `reconnect pid=... code=0 status=WAITING`을 출력하고 첫 유효 Snapshot부터
   새로운 비교 기준을 설정합니다. 새 세대의 낮은 Sequence도 정상적으로 수용합니다.

정상 송수신 중 `r`을 입력해도 동일한 절차를 수행하며 통계가 초기화됩니다.
SHM 부재 등으로 실패하면 `status=DISCONNECTED code=...`를 출력하고 Observer는 살아 있습니다.
이전 Snapshot은 폐기되어 Sequence/값/Age를 유효 데이터처럼 출력하지 않습니다.
대상이 다시 생겨도 자동 재시도하지 않으므로 **다시 `r`을 입력해야 합니다.**
Create 성공 후 아직 데이터가 없으면 `WAITING`이며, 오래된 데이터면 기존 Age 기준으로
`STALE`입니다. 연결 성공 자체를 데이터 Freshness로 취급하지 않습니다.

stdin EOF/닫힘에서는 명령 입력만 중지하며 기존 데이터 관측은 계속됩니다.
이 경우 다시 입력 가능한 stdin이 없으므로 사용자 명시적 Observer 재실행이 필요합니다.
정상 Shutdown에서는 stdin의 기존 파일 상태 플래그를 복원합니다.

## 모두 Standalone 실행

각각 별도 터미널에서 실행합니다. Bringup 실행과 INSTANCE를 공유하지 않습니다.

```bash
./kcf_composition_source standalone_01
./kcf_composition_processor standalone_01
./kcf_composition_observer standalone_01
```

세 프로세스 모두 `mode=Standalone`입니다. 종료는 Observer → Processor → Source 순서로
각 터미널의 Ctrl+C를 사용합니다. Standalone 자식 간에는 프로세스 관리 관계가 없습니다.

## 데이터 및 연결 계약

- Topic 이름: `/kcf_composition_INSTANCE_source`, `/kcf_composition_INSTANCE_result`.
- 기본 depth=1/latest-value. 속도가 서로 다르므로 중간 Sequence 건너뛰기는 정상입니다.
  `skipped`는 관측한 원본 Sequence 사이의 차이이며 전송 오류 개수가 아닙니다.
- `repeated`는 동일 Transport Sequence의 Snapshot을 재조회한 횟수입니다. 새 Publish의
  중복 Payload Sequence(`duplicates`) 및 역순(`reversed`)과 구분합니다.
- Processor는 새 Snapshot만 가공합니다. 읽기와 쓰기가 `-EAGAIN`이면 해당 cycle을 건너뜁니다.
  그 외 API 오류와 잘못된 값/Sequence는 Runtime에 오류로 반환합니다.
- Observer Age는 원본 Source timestamp부터 계산합니다. `NEW`, `UNCHANGED`, `WAITING`,
  `STALE`을 표시하며, 500ms는 이 예제의 표시 기준일 뿐 Framework timeout 정책이 아닙니다.
- Callback 대신 기존 Subscriber의 pull `ReadLatest`를 사용해 각 Loop에서 Snapshot을 읽습니다.
  Source/Processor는 약 1초마다 통계, Observer는 매 Loop마다 상태를 출력합니다.
- 입력 Topic이 아직 없으면 Setup에서 최대 3초 대기합니다. 이 범위 내 실행 순서 역전을 허용하며,
  이후에는 `-ETIMEDOUT`으로 종료합니다. 무한 재시도·자동 재연결·재시작은 없습니다.
- 같은 INSTANCE의 살아 있는 Publisher와 충돌하면 Create 실패로 종료합니다.
  다른 실행의 Topic을 삭제하지 않습니다. Producer 재생성 후 기존 reader를 새 세대로
  자동 연결하지 않습니다. Observer는 사용자가 `r`로 재연결할 수 있으며,
  Processor 등 다른 소비자는 필요하면 사용자가 명시적으로 다시 실행해야 합니다.

## 종료 및 장애 확인

정상 Shutdown에서는 Subscriber Close만 수행하고, 각 Publisher는 자신이 생성한 Topic만
Close/Unlink합니다. Source와 Processor는 각각 자신의 Topic만 소유합니다.

관리 프로세스의 장애 확인은 Bringup의 `started name=source pid=...`에 나온 PID에
`kill -KILL PID`를 보내 수행할 수 있습니다. 기존 Supervisor가 `RUNNING -> ERROR`와
원인을 기록하며 자동 재시작하지 않습니다. 이 예제는 ERROR 시 Application의 안전 제어
정책을 추가하지 않습니다. 다른 관리 프로세스는 기존 Bringup 정책에 따릅니다.
시험 종료는 Bringup에 SIGTERM/Ctrl+C를 전달합니다.

SIGKILL은 Shutdown을 실행하지 않으므로 종료된 Publisher의 SHM이 남을 수 있습니다.
예제에서 이를 임의 삭제하지 않습니다. 기존 Framework의 dead-owner 검증/회수 기능을
사용하도록 해당 Publisher를 같은 INSTANCE로 수동 재실행한 뒤 정상 종료할 수 있습니다.
이것은 자동 복구 정책이 아닙니다.

## 검증

개발 환경의 격리된 SHM/IPC namespace에서 다음을 확인했습니다.

- Debug 빌드, 각 Element standalone, 입력 연결 순서 역전 및 3초 타임아웃
- Topic 이름 충돌 시 기존 owner 보호
- 100/50/10Hz 설정에서 데이터 가공·Sequence·Age 확인, 중복/역순 0
- Source 일시 정지 시 UNCHANGED/STALE 표시 및 동일 프로세스 재개 후 NEW
- Bringup의 두 Managed Element RUNNING 및 External Observer 종료 독립성
- Bringup 종료 후 Observer 생존, Managed 자식 회수와 정상 Topic SHM 정리
- Managed Source SIGKILL 시 ERROR latch, 자동 재시작 없음 및 명시적 stale 회수
- 동일 PID에서 정상 실행 중 반복 `r`, 정상/장애 재시작 후 새 세대로 명시적 재연결
- SHM 부재 시 DISCONNECTED, 이전 Snapshot 폐기, 자동 재시도 없음
- 재연결 후 Sequence/Timestamp 비교 기준 초기화, Freshness 및 중복/역순 0
- 반복 재연결의 FD/매핑 수 유지 및 종료 시 stdin 플래그 복원
- 기존 Runtime lifecycle/Supervision, Bringup Application scope 및 Topic queue/recovery 회귀

설정 주파수는 hard real-time 보장이 아닙니다. 별도 실장/RT 성능 검증은 포함하지 않습니다.
