# KCF Framework v5.1 Status

기준 브랜치: `dev`. Framework **v5.1**, CMake project version: **5.1.0**.

2026-09-19 읽기 전용 확인 결과, GitHub dev와 로컬 HEAD는 모두
[82fa3b274f51d847602b2a5e4936778ebd631257](https://github.com/Nyamkani/kss_control_framework/commit/82fa3b274f51d847602b2a5e4936778ebd631257)
(`v5.1: extend Topic with bounded queue support`)입니다. v5.1 코드·문서는 이미 commit/push되어 있습니다.
`git ls-remote origin refs/heads/dev 'refs/tags/*'`에서 원격 tag는 없었으며,
[GitHub Releases API](https://api.github.com/repos/Nyamkani/kss_control_framework/releases)는 빈 목록을 반환했습니다.
확인 시점에 원격 tag와 공개 Release는 없으며 버전 반영 commit과 구분합니다.
[v5.0 checkpoint](V5_0_STATUS.md)는 당시 구현·검증 이력으로 보존합니다.

## Topic Queue 계약

- Depth 기반 Bounded Ring Buffer. 기본 Depth는 1이며 기존 Create 호출을 유지합니다.
- Depth=1 Snapshot, Depth=N 최근 N개 메시지 KEEP_LAST. Publisher가 Depth를 설정하고 Subscriber는 SHM Header에서 확인합니다.
- Subscriber별 독립 Cursor. 기본 NEXT는 연결 이후, OLDEST는 보관된 가장 오래된 메시지부터 순차 소비합니다.
- ReadNext 성공 시에만 Cursor를 이동합니다. ReadLatest와 기존 Callback은 최신값 방식이며 순차 Cursor에 영향을 주지 않습니다.
- Overflow 누락 수는 `TopicReadInfo::missed`, Transport Sequence는 `TopicReadInfo::sequence`로 제공합니다.
- Transport Sequence는 64-bit이며 Commit 성공 시에만 증가합니다. 실패한 Publish는 Sequence와 Queue를 변경하지 않습니다.
- 안전한 Write Slot 부족은 `-EAGAIN`. ReadNext의 미처리 메시지 부재·복사 경합도 `-EAGAIN`이며 Cursor는 유지합니다.
- Payload 측정 Sequence·Timestamp·Validity는 Application 책임입니다. `valid=false`도 그대로 전달합니다.

API 예제는 [개발 가이드](05_APPLICATION_DEVELOPMENT_GUIDE.md), 세부 계약은 [IPC](03_IPC_AND_FEATURES.md#topic)를 참조합니다.

## SHM 호환성 및 제한

**Topic SHM Format 4**, **Parameter SHM Format 3 유지**.
기존 Topic Format 3과 바이너리 호환되지 않으므로 Publisher/Subscriber와 Tool 등 참여 바이너리를
호환 Core 및 Payload ABI로 재빌드해야 합니다. 기존 Topic SHM을 임의 삭제하거나 자동 변환하지 않습니다.
이전 세대 종료와 소유자 정리·재생성 절차가 필요합니다.

논리 Entry마다 Triple Buffer의 pin/CAS 보호를 유지하여 읽는 Slot을 강제로 덮어쓰지 않습니다.
단일 Publisher/쓰기 thread, dead-reader pin 잔존, 명시적 재연결 제한은 유지합니다.
Typed Subscriber는 Close/Create, SharedChannel과 DynamicTopicReader는 Close/Open으로 재연결합니다.
DynamicTopicReader는 SHM 교체를 `-ESTALE`로 알리며 Depth와 무관하게 최신값을 조회합니다.
KEEP_LAST는 유실 없는 전달 보장이 아니며 Depth당 3개 Payload Slot과 metadata가 필요합니다.
Sequence 최대값에서는 Publish가 `-EOVERFLOW`를 반환합니다.

## 검증 이력 구분

기존 v5.0의 22개 회귀 기록, Queue 구현 단계의 보고된 10개 영역/19개 회귀/Tool Echo 결과와
v5.1 구현 완료 단계의 최종 실행을 구분합니다. [최종 검증 기록](06_VERIFICATION_AND_LIMITATIONS.md#v51-final-verification)에
실행 명령·결과·범위를 기록합니다. 하드웨어·hard real-time·GUI 수동 재검증을 의미하지 않습니다.

## v5.1 최종 결과

2026-09-19 v5.1 구현 완료 당시 dev 작업 트리에서 실제 실행한 기록:

- 새 build/v5.1 디렉터리의 C++17 Debug 전체 configure/build: PASS.
- 신규 Queue 필수 10개 영역: PASS.
- 기존 회귀 runner 19개 + 추가 Runtime protocol/Application scope/Timer 3개: **22/22 PASS**.
- 기존 kcf_tools Backend/Topic Echo: **1/1 PASS** (Tool 소스 변경 없이 새 Core로 재빌드).

기능 구현 변경을 추가하지 않고 기존 Queue 코드를 최종 검증했습니다.
위 결과는 v5.1 구현 검증 시점의 기록입니다. 이후 GitHub 반영은 상단 commit으로 확인했습니다.
이번 README 통합에서는 문서와 링크만 검사했으며 기능 테스트를 재실행하지 않았습니다.
Mecanum Application은 현재 Framework repository에 포함되지 않습니다.
