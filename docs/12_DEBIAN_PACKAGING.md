# KCF Core v5.6.1 Debian 패키징 및 배포 준비

v5.6.1은 Ubuntu 24.04 amd64 Release DEB 생성·추출 실행·SDK 검증을 완료했다.
실제 설치·업그레이드 및 GitHub Release 업로드는 별도 단계다.
기존 [v5.6.0 Pre-release](https://github.com/Nyamkani/kss_control_framework/releases/tag/v5.6.0)는
Ubuntu별 DEB와 정적 CMake SDK를 이미 배포했다. 아래 과거 VM 기록은 당시 증거로 보존한다.

## 지원 환경과 검증 범위

| 환경 | v5.6.0 Release 기록 | 신규 v5.6.1 검증 상태 |
| --- | --- | --- |
| Ubuntu 24.04 amd64 | 패키징·압축 해제·SDK PASS | Debug 회귀 54/54 및 Release DEB·추출 SDK PASS; 실제 설치/운영 미검증 |
| Ubuntu 24.04 arm64 | Native Build·설치·SDK PASS | 빌드·패키징·실물 검증 미완료 |
| Ubuntu 26.04 arm64 | OPI5+ Native Build·설치·SDK PASS | 빌드·패키징·실물 검증 미완료 |

과거 Release의 ARM64 PASS를 신규 변경에 승계하지 않는다. OPI5+의 UGV02 재빌드·실물 동작도
미완료다. 동일 ARM64라도 Ubuntu별로 별도 Native Build·검증한다.

패키징을 켰을 때만 `/etc/os-release`의 ID/VERSION_ID를 읽고, 파일이 없으면
`/usr/lib/os-release`를 사용한다. `dpkg --print-architecture`, 대상 CPU 및 64-bit ABI를
대조한다. 미지원 조합, 불명확한 환경, 교차 컴파일은 거부하며 일반 개발 빌드에는 이 제한을 적용하지 않는다.
Upstream은 `5.6.1`, revision은 `1+ubuntu${VERSION_ID}`다. Release 산출물 명명 예:

- `kcf-core_5.6.1-1+ubuntu24.04_amd64.deb`
- `kcf-core_5.6.1-1+ubuntu24.04_arm64.deb`
- `kcf-core_5.6.1-1+ubuntu26.04_arm64.deb`

24.04 amd64는 아래 결과의 실제 산출물이며 ARM64 이름은 예정 형식이다.
suffix가 설치 OS를 강제로 제한하지는 않는다.
가상 입력 환경 검사 PASS는 ARM64 빌드 PASS가 아니다.

## 빌드·패키지 검사 (Ubuntu 24.04 amd64 예시)

CMake, C++17 compiler, make, dpkg-dev가 필요하다. 저장소 루트에서 실행한다.
다음은 Release 패키징 재현 예시이며 이번 문서 작업에서는 실행하지 않았다.
실제 산출물과 로그는 아래 최종 결과 경로에 보존하며, 재현 시 별도 출력 경로를 사용한다.
개발용 `build-main`은 Debug로 유지한다.

```sh
cmake -S . -B /tmp/kcf-561-deb-build \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr \
  -DBUILD_SHARED_LIBS=OFF -DKCF_ENABLE_PACKAGING=ON
cmake --build /tmp/kcf-561-deb-build -j2
cpack --config /tmp/kcf-561-deb-build/CPackConfig.cmake -B /tmp/kcf-561-debs
dpkg-deb --info /tmp/kcf-561-debs/kcf-core_5.6.1-1+ubuntu24.04_amd64.deb
dpkg-deb --contents /tmp/kcf-561-debs/kcf-core_5.6.1-1+ubuntu24.04_amd64.deb
dpkg-deb --field /tmp/kcf-561-debs/kcf-core_5.6.1-1+ubuntu24.04_amd64.deb Version Architecture Depends
dpkg-deb --extract /tmp/kcf-561-debs/kcf-core_5.6.1-1+ubuntu24.04_amd64.deb /tmp/kcf-561-extracted
sha256sum /tmp/kcf-561-debs/kcf-core_5.6.1-1+ubuntu24.04_amd64.deb
readelf -d /tmp/kcf-561-extracted/usr/bin/kcf_gateway
```

Version/Architecture와 파일명이 일치하는지 확인한다.
ELF NEEDED와 최종 Depends, 설치 경로·실행 권한 및 RPATH/RUNPATH를 검사한다.
시스템 설치 없이 검사하며, ARM64 패키지를 만들기 위해 아키텍처 문자열만 바꾸지 않는다.

## 설치 파일 및 의존성

`kcf-core`는 Runtime/개발 SDK를 합친 단일 정적 패키지다. 기본 `/usr` 설치 구성:

| 경로 | 내용 |
| --- | --- |
| `/usr/bin/kcf_gateway`, `/usr/bin/kcf_bringup` | 실행파일 |
| `/usr/sbin/kcf-gateway-setup` | 실행 가능한 Python 설정 도구 |
| `/usr/share/kcf/systemd/kcf-gateway.service.in` | 계정/실행 경로 placeholder가 남은 템플릿 |
| `/usr/share/kcf/systemd/gateway.env.example` | 배포용 설정 예제 |
| `/usr/lib/<multiarch>` | `libkcf_core.a`, `libkcf_bringup_support.a` |
| `/usr/include/kcf`, `/usr/include/bringup` | 공개 및 필요한 detail 헤더 |
| `/usr/lib/<multiarch>/cmake/KCF` | config/version/target export |

GNUInstallDirs를 사용하며 실제 multiarch 경로는 빌드 환경에서 확인한다.
예제·테스트 binary, 활성 `/etc/systemd/system/kcf-gateway.service`, `/etc/kcf/gateway.env`,
Host ID는 패키지 설치 대상이 아니다. README 운영 안내는 저장소 문서이며 패키지에 자동 설치되지 않는다.
서비스를 제어하는 maintainer script도 없다. 템플릿을 배포하는 것과 활성 Unit 설치는 별개다.

`CPACK_DEBIAN_PACKAGE_SHLIBDEPS=ON`으로 ELF 런타임 의존성을 빌드 환경의 symbols/shlibs에서
산출한다. glibc/libstdc++ 버전을 고정하지 않으며 시스템 라이브러리를 복사하지 않는다.
설정 도구의 `python3`는 명시 의존성이다. 운영 호스트의 systemd/systemd-analyze는 도구 사용 전제다.
정적 SDK의 최종 링크와 compiler/CMake 등 개발 도구 요구는 shlibdeps만으로 검증되지 않는다.

## SDK 소비자

```cmake
find_package(KCF 5.6.1 EXACT CONFIG REQUIRED)
target_link_libraries(my_application PRIVATE KCF::core)
# Bringup API 소비자는 KCF::bringup 사용
```

`KCF::core`는 C++17/헤더/Threads를, `KCF::bringup`은 Core와 Threads를 전이 제공한다.
Config 검색은 SameMinorVersion이며 EXACT는 정확한 버전을 요구한다. 이는 ABI나 Network wire
호환성 보증이 아니다. 기존 `kcf_core` 및 `bringup.cpp` 직접 소스 링크 방식은 유지한다.

```sh
cmake -S tests/packaging/consumer -B /tmp/kcf-561-consumer \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/tmp/kcf-561-extracted/usr
cmake --build /tmp/kcf-561-consumer
/tmp/kcf-561-consumer/core_consumer
/tmp/kcf-561-consumer/bringup_consumer
```

소비자 fixture는 자식·Gateway·하드웨어를 시작하지 않는다. `PeerInfo.last_complete_snapshot`
추가로 공개 구조 레이아웃이 바뀌었다. 새 헤더와 archive를 함께 사용해 소비자를 재컴파일·재링크한다.
정적 링크된 Application/Tools에는 Core 패키지 교체만으로 수정이 반영되지 않는다.

## 설치·업그레이드 및 Gateway 보존 정책

아래는 관리자가 승인한 시험 환경에서만 수행할 후속 절차다. 실제 설치·업그레이드·제거는 미검증이다.
기존 서비스 상태/UID, Unit·환경 설정, `/var/lib/kcf/host-id`의 내용·소유권을 먼저 기록하고
운영 데이터 백업 정책에 따라 보존한다. 아래 패키지명은 24.04 amd64 예시다.

```sh
sudo apt install ./kcf-core_5.6.1-1+ubuntu24.04_amd64.deb
sudo /usr/sbin/kcf-gateway-setup --user KCF_USER --dry-run
# 기존에 설정 도구로 설치한 Unit 갱신 승인
sudo /usr/sbin/kcf-gateway-setup --user KCF_USER --update
```

패키지를 내려받은 디렉터리에서 실행하며 OS/아키텍처 및 제공된 SHA256SUMS를 먼저 확인한다.
5.6.0 → 5.6.1은 버전 업그레이드다. 같은 Version의 패키지를 다시 생성한 경우에는
`sudo apt install --reinstall ./<package>.deb`로 명시적 재설치가 필요할 수 있다.

최초 Unit 설치는 `--user KCF_USER`로 수행하고, 활성화는 별도 승인 `--enable`이 필요하다.
root/빌드 계정을 자동 선택하지 않으며 KCF Application과 동일한 일반 로그인 UID를 사용한다.
Unit 갱신은 알려진 과거 템플릿만 백업 후 교체하고 동일 Unit은 유지한다. reload 실패 후 동일한
현재 Unit으로 재시도하면 검증 후 reload만 수행한다. 실제 서비스 재시작은 별도 관리자 승인 절차다.
[상세 CLI·복구·재시작 정책](../gateway/systemd/README.md)을 따른다.

Host ID 재생성, 임의 chown, Lock/Socket/SHM 삭제, 수동 Gateway 강제 종료는 하지 않는다.
기존 사용자·경로·소유권 충돌 및 관리자 작성 Unit은 자동으로 덮어쓰지 않는다.
`/etc/kcf/gateway.env`는 복사/덮어쓰지 않으며 비기본 설정은 수동 검토가 필요하다.
패키지는 위 운영 파일을 소유하지 않으므로 제거가 서비스 정리 절차를 대신하지 않는다.
제거/계정 이전/이미지 복제의 운영 절차는 별도 검증 대상으로 남는다.

<a id="v561-final-results"></a>
## v5.6.1 최종 회귀 및 Release 패키징 결과 (2026-11-23)

| 검증 | 결과 및 범위 |
| --- | --- |
| Core `build-main` Debug 증분 빌드 | PASS |
| Core Debug 개별 회귀 | **54/54 PASS**; 테스트/스위트 실행 단위 집계 |
| Core CTest | 등록 **0개 — SKIP**; 전체 테스트 PASS로 집계하지 않음 |
| Network 계약·호환성, PeerDiscovery/Metadata Snapshot | PASS |
| Gateway Lifecycle·Local IPC·Integration | PASS |
| Gateway systemd 설정 도구 | mock **25/25 PASS**; 위 회귀의 스위트 1개, 실제 서비스 변경 없음 |
| Virtual LAN 전체 | PASS; 링크 DOWN/UP **15초** 조건 유지, UDP 손실·재정렬·지연, SHM 복구·Gateway 재시작 포함 |
| Debug SDK 소비자 | 5.6.1 EXACT configure/build/link/run **2종 PASS** |
| Release CMake configure/build 및 CPack | PASS; Ubuntu 24.04 amd64, 정적 SDK |
| DEB info/contents·추출 실행 | PASS; Gateway 도움말, Bringup Usage, 설정 도구 도움말 |
| 추출 Release SDK 소비자 | configure/build/link/run **2종 PASS** |
| SHA256 생성·재검증 | PASS |
| 실제 설치·업그레이드·제거, systemd 운영 복구·부팅 | 미검증 |
| 실제 Wi-Fi/Core #6, ARM64/OPI5+, UGV02 재빌드·실물 동작 | 미검증 |

54/54는 Debug 결과이며 Release 전체 회귀는 수행하지 않았다. 최초 Virtual LAN FAIL 기록
(`/tmp/kcf-final-561-015`)과 진단 기록은 보존하며 이번 PASS로 대체하지 않는다.
NetworkProtocol 1.0/Discovery v2/Control TCP v1/Topic wire v1/Local Gateway IPC v1 및 SHM 형식은 유지한다.

최종 DEB:

- 환경: **Ubuntu 24.04 amd64 / Release**, upstream `5.6.1`, revision `1+ubuntu24.04`.
- 파일: `kcf-core_5.6.1-1+ubuntu24.04_amd64.deb` — **1,131,794 bytes**.
- 경로: `/tmp/kcf-core-561-release/packages/kcf-core_5.6.1-1+ubuntu24.04_amd64.deb`.
- SHA256: `b057ef8c02e600aa563b69f7f1a5c5dc7c9168c89b6910b35ccbeef22b710fa5`.
- Depends: `python3, libc6 (>= 2.38), libgcc-s1 (>= 3.0), libstdc++6 (>= 13.1)`.
  이는 해당 산출물의 실제 값이며 CMake에 고정한 의존성 버전이 아니다.
- 파일 81개: 실행파일 2, 설정 도구 1, 서비스 템플릿·환경 예제 2, 정적 archive 2,
  헤더 70, CMake config/export 4. library/config 경로는 `/usr/lib/x86_64-linux-gnu` 및 `cmake/KCF`.
- 실행파일·설정 도구 0755, 템플릿·환경 예제 0644. 활성 Unit, Host ID, 사용자 설정,
  예제·테스트 binary 및 maintainer script 없음. 자동 서비스 활성화 없음, 실행파일 RPATH/RUNPATH 없음.

회귀 기록은 `/tmp/kcf-core-final-561-8212zif3/RESULT.md`와 `results.json`, 패키징 기록은
`/tmp/kcf-core-561-release/`의 `configure.log`, `build.log`, `cpack.log`, `deb-info.txt`,
`deb-contents.txt`, `verification.json` 및 `packages/SHA256SUMS`에 보존했다.
`/tmp`는 영구 보관을 보장하지 않으므로 Release 업로드 시 산출물·체크섬·증거를 별도로 보관한다.
이번 문서 갱신에서는 빌드·회귀·패키징을 재실행하지 않았다.
기존 설치 Core v5.6.0, 운영 Gateway, Host ID 및 개발용 Debug 빌드는 변경하지 않았다.

## 2026-11-11 개발 VM 검증

기준은 `dev` / `570a233`에 이 패키징 변경을 적용한 작업 트리다. Ubuntu 24.04,
`dpkg --print-architecture=amd64`, `uname -m=x86_64`, CMake 3.28.3, Debug로 검증했다.
ARM64 빌드나 실제 설치·운영 PASS가 아니다.

| 검증 | 결과 |
| --- | --- |
| Core 전체 Debug configure/build | PASS |
| 기존 stabilization smoke의 Core 테스트 | 11/11 PASS |
| 별도 Network contract/compatibility/discovery, Runtime/Supervisor, IPC/Timer 회귀 | 12/12 PASS |
| 두 저장소 smoke 전체 | 테스트 35/35 PASS; 준비·빌드 등을 포함하면 41/41 단계 PASS |
| CPack, dpkg-deb info/contents, 임시 압축 해제 | PASS |
| 최종 DEB의 외부 소비자 2종 configure/build/link/실행 | PASS |
| Shared Core의 1단계 DEB 생성 거부 | PASS (의도된 configure 실패) |

별도 실행한 과거 `r41_regressions.py`는 앞선 9개 PASS 후 `runtime_health`에서 FAIL했다.
`examples/supervisor/runtime_health_checks.py:130`은 초기 실패 후 `Origin name=`이 없어야 한다고
검사하며 현행 v5.5 이후 초기 ERROR 유지 계약과 맞지 않는다. runner가 여기서 중단했으므로
그 뒤 항목을 이 runner의 PASS로 기록하지 않는다. 테스트/Runtime 계약은 이번에 수정하지 않았다.

원래의 `main.cpp + bringup.cpp` 직접 링크 방식으로 만든 비교 실행파일의 전체 구형 health 시험도
FAIL했으며, 이 시도에서는 동시 stdout 혼합으로 PID 로그 파싱이 먼저 실패했다. 별도 초기 실패
시나리오 비교에서는 직접 링크 및 패키지 실행파일 모두 ERROR/Origin을 게시하고 명시적 Shutdown까지
살아 있는 동일한 동작을 확인했다. 현행 Supervisor Failure Contract는 PASS다.

최종 패키지:

- `/tmp/kcf-deb-phase1/final/core/kcf-core_5.6.0-1_amd64.deb`
- SHA-256: `9aaea8f297d99f0f47df952ce65fae74737905593b8d4d2964b7513058cd29d8`
- 파일 78개: 실행파일 2, 정적 archive 2, 헤더 70, CMake config/export 4.
- 실제 library/config 경로: `/usr/lib/x86_64-linux-gnu` 및 그 아래 `cmake/KCF`.
- Depends: `libc6 (>= 2.38), libgcc-s1 (>= 3.0), libstdc++6 (>= 13.1)`.
- maintainer script, 서비스 unit, 예제/테스트, Host ID/설정 파일 없음. 설치 실행파일 RPATH/RUNPATH 없음.

로그는 `/tmp/kcf-deb-phase1/`의 `core-*-configure.log`, `core-build.log`,
`core-final-cpack.log`, `core-final-info.txt`, `core-final-contents.txt`,
`core-regressions.log`, `core-extra.json`, `startup-comparison.json`,
`smoke/results.json`, `final-consumer-*.log`, `final-artifacts.json`에 보존했다.
임시 산출물이므로 영구 보관을 보장하지 않는다. 시스템 설치, systemd 운영, 실제 LAN/하드웨어,
Mecanum/UGV02 수정 및 commit/push/tag/release는 수행하지 않았다.
