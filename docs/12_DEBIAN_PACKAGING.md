# Debian 패키징 1단계

## 2단계: Ubuntu별 패키징 환경과 산출물

패키징을 활성화할 때만 `/etc/os-release`의 `ID`/`VERSION_ID`를 읽으며, 파일이 없을 때만
`/usr/lib/os-release`를 사용한다. `dpkg --print-architecture`의 성공과 아키텍처,
대상 CPU/64-bit ABI를 검사한다. 값이 없거나 지원하지 않으면 추정하지 않고 오류로 종료한다.
교차 컴파일 패키징은 거부하며 일반 개발 빌드는 이 지원 환경 제한을 적용하지 않는다.

| Core 지원 조합 | 실제 패키징 검증 상태 |
| --- | --- |
| Ubuntu 24.04 amd64 | 개발 VM에서 검증 |
| Ubuntu 24.04 arm64 | 실제 빌드·검증 미완료 |
| Ubuntu 26.04 arm64 | 실제 환경 없음, 빌드·검증 미완료 |

Upstream version은 `5.6.0`으로 유지하며 Debian revision은 `1+ubuntu${VERSION_ID}`로 자동 생성한다.
현재 규칙은 아래 1단계 이력의 `-1` 파일명 대신 `-1+ubuntu24.04` 등을 사용한다.
GitHub Release 산출물 이름은 `<package>_<upstream>-<revision>_<architecture>.deb`이며 예시는 다음과 같다.

- `kcf-core_5.6.0-1+ubuntu24.04_amd64.deb`
- `kcf-core_5.6.0-1+ubuntu24.04_arm64.deb` (예정, 실제 검증 미완료)
- `kcf-core_5.6.0-1+ubuntu26.04_arm64.deb` (예정, 실제 검증 미완료)

동일 ARM64라도 배포판별로 따로 빌드·검증한다. suffix는 설치 대상 OS를 강제로 제한하지 않는다.
가상 입력의 지원 조합 검사는 실제 ARM64 빌드 성공을 의미하지 않는다. Release 업로드는 별도 사용자 작업이다.

`CPACK_DEBIAN_PACKAGE_SHLIBDEPS=ON`으로 실행파일이 사용하는 런타임 라이브러리의 최소 의존성을
대상 빌드 환경의 symbols/shlibs 정보에서 산출한다. glibc/libstdc++ 버전은 고정하지 않는다.
정적 SDK 소비자의 최종 링크 요구사항과 compiler/CMake 등 개발 도구 요구는 이 분석만으로
검증되지 않으므로 각 대상 환경에서 외부 SDK 소비자 configure/link 검증을 수행해야 한다.

Core v5.6.0의 설치 SDK와 CLI를 `kcf-core` 단일 DEB로 제공한다. 이 단계는
런타임/개발 패키지를 분리하지 않으며, 기본 정적 라이브러리를 사용한다.
Application, Network/IPC/Runtime 계약 및 systemd 운영 정책은 변경하지 않는다.

## 빌드와 패키지 생성

Debian/Ubuntu의 CMake, C++17 compiler, make, dpkg-dev가 필요하다.
아래 명령은 시스템 설치를 수행하지 않는다. Debug는 개발 검증용이며 배포 최적화 빌드와 구분한다.

```sh
cmake -S . -B /tmp/kcf-deb-build \
  -DCMAKE_BUILD_TYPE=Debug -DCMAKE_INSTALL_PREFIX=/usr \
  -DBUILD_SHARED_LIBS=OFF -DKCF_ENABLE_PACKAGING=ON
cmake --build /tmp/kcf-deb-build -j2
cpack --config /tmp/kcf-deb-build/CPackConfig.cmake -B /tmp/kcf-debs
dpkg-deb --info /tmp/kcf-debs/kcf-core_5.6.0-1_amd64.deb
dpkg-deb --contents /tmp/kcf-debs/kcf-core_5.6.0-1_amd64.deb
dpkg-deb --extract /tmp/kcf-debs/kcf-core_5.6.0-1_amd64.deb /tmp/kcf-extracted
```

파일명의 아키텍처는 실행한 Debian 빌드 환경에서 결정된다. 위 `amd64`는 예시이며,
Orange Pi 5+용 `arm64`는 해당 아키텍처/배포판의 별도 빌드·검증이 필요하다.
단순히 CPack 아키텍처 문자열을 바꾸는 것으로 교차 빌드하지 않는다.

## 설치 내용과 외부 소비자

| 경로 | 내용 |
| --- | --- |
| `/usr/bin` | `kcf_bringup`, `kcf_gateway` |
| `/usr/lib/<multiarch>` | `libkcf_core.a`, `libkcf_bringup_support.a` |
| `/usr/include/kcf`, `/usr/include/bringup` | 공개 헤더 및 공개 헤더가 참조하는 detail 헤더 |
| `/usr/lib/<multiarch>/cmake/KCF` | config, version 및 target export |

GNUInstallDirs를 사용한다. multiarch 디렉터리의 실제 값은 CMake/toolchain에 따른다.
예제·테스트 binary, systemd unit/maintainer script, Host ID, 설정 파일은 설치하지 않는다.
시스템 라이브러리는 복사하지 않으며 DEB 실행파일 의존성은 `dpkg-shlibdeps`로 산출한다.

```cmake
cmake_minimum_required(VERSION 3.16)
project(my_application LANGUAGES CXX)
find_package(KCF 5.6.0 EXACT CONFIG REQUIRED)
add_executable(my_element main.cpp)
target_link_libraries(my_element PRIVATE KCF::core)
# Bringup API를 사용하는 별도 실행파일에는 KCF::bringup을 링크한다.
# KCF::bringup은 KCF::core와 Threads::Threads를 전이 제공한다.
```

`KCF::core`는 C++17과 헤더 경로, Threads 의존성을 전이 제공한다.
Config version은 동일 minor 범위의 호환 검색을 제공하며, 정확한 SDK 고정에는 `EXACT`를 사용한다.
이는 Network의 버전 정책이나 wire 호환성 선언이 아니다.

설치하지 않고 외부 프로젝트 configure/link 및 실행을 확인하는 fixture:

```sh
cmake -S tests/packaging/consumer -B /tmp/kcf-consumer \
  -DCMAKE_PREFIX_PATH=/tmp/kcf-extracted/usr
cmake --build /tmp/kcf-consumer
/tmp/kcf-consumer/core_consumer
/tmp/kcf-consumer/bringup_consumer
```

fixture는 자식 프로세스·Gateway·하드웨어를 시작하지 않는다. 기존 `kcf_core` target 및
`bringup.cpp` 직접 소스 링크 방식은 유지한다. `KCF_ENABLE_PACKAGING` 기본값은 OFF이며
기존 빌드에 CPack 실행을 추가하지 않는다. 정적 Core를 포함한 Application/Tools는 Core DEB만
교체해도 갱신되지 않으므로 해당 실행파일의 재빌드·재배포가 필요하다.

KCF Tools는 별도 저장소의 `docs/DEBIAN_PACKAGING.md`를 따른다.
실제 시스템 설치·운영, ARM64, systemd 부팅/재시작 검증은 후속 단계다.

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
