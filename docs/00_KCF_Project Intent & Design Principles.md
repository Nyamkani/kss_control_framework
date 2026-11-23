# KCF — Project Intent & Design Principles

## 1. 개발 목적

KCF(KSS Control Framework)는 Linux 기반 제어 Application을 개발할 때 반복적으로 필요한 실행 구조, 프로세스 관리 및 데이터 통신 기능을 공통화하기 위한 C++ Framework이다.

ROS/ROS2와 같은 범용 로봇 미들웨어의 모든 기능을 구현하는 것이 목표는 아니다.

복잡한 설정이나 외부 프레임워크에 대한 의존을 줄이고, 개발자가 센서, 모터, 제어 알고리즘 및 Application 로직 구현에 집중할 수 있는 환경을 제공하는 것이 주목적이다.

## 2. 기본 Application 구조

KCF는 기능별 독립 프로세스 구성을 기본으로 한다.

- **Element:** 센서, 모터, Controller 등 하나의 주요 기능을 담당하는 독립 프로세스
- **ProcessRuntime:** Element의 Setup, 주기적 Loop, Shutdown 및 실행 상태 관리
- **Supervisor / Bringup:** 다수 Element의 실행, 상태 감시, 오류 관리 및 종료·복구
- **Application:** 여러 Element를 조합하여 하나의 제어 시스템을 구성

Element 내부의 Driver, Algorithm, FSM 등은 개발자가 자유롭게 구성하며, Framework는 실제 제어 로직을 대신하지 않는다.

각 Element는 독립 개발·실행·검증이 가능해야 하며, 동일한 실행 파일을 단독으로 실행하거나 Supervisor를 통해 통합 실행할 수 있도록 한다.

## 3. Local IPC 설계

독립 프로세스 간 데이터 교환을 위해 공통 인터페이스를 제공한다.

- **Topic:** 주기적 데이터 및 상태 전달
- **Parameter:** 설정값 조회 및 변경
- **Service:** 요청 및 응답
- **Action:** 장시간 동작 요청, 진행 상태 및 결과 관리

Local IPC는 개별 Element 사이에서 직접 사용한다.

Supervisor는 프로세스를 관리하지만, 사용자 데이터의 중앙 중계기 역할을 하지 않는다.

## 4. Network Gateway 설계

KCF의 Local IPC와 Network 통신은 별도의 영역으로 구분한다.

Network는 Local IPC 자체를 원격으로 그대로 확장하는 구조가 아니라, **Gateway를 원격 통신의 진입점으로 사용**하는 구조이다.

- Local Element 간 통신은 Gateway 없이 동작한다.
- Gateway는 Local Application과 외부 Network 사이의 연결 경계이다.
- Discovery를 통해 원격 Host 및 Endpoint 정보를 탐색한다.
- 외부에 필요한 Endpoint만 선택적으로 노출한다.
- 원격 통신 및 제어 권한은 별도로 관리한다.
- Network 장애가 Local IPC의 기본 실행을 직접적으로 종속시키지 않도록 한다.

따라서 Gateway는 모든 Local IPC 데이터를 자동으로 외부에 중계하는 중앙 Broker가 아니다.

## 5. Framework와 Application의 책임

**Framework 책임**
- 프로세스 실행 및 Lifecycle 관리
- 실행 주기 관리
- 프로세스 상태 감시
- 공통 IPC 제공
- 선택적 Network 통신 기반 제공
- 실행 구조 및 Endpoint 정보 관측

**Application 책임**
- 하드웨어 Driver 및 Interface
- 제어 알고리즘
- 상태 추정 및 FSM
- 데이터 유효성 판단
- 장치별 안전 정책
- 실제 동작 명령 및 출력

## 6. 개발 방향

KCF의 목적은 기능 수를 최대한 늘리는 것이 아니라, 필요한 기능을 일관된 인터페이스로 제공하여 제어 Application 개발 부담을 줄이는 것이다.

Topic, Parameter, Service, Action을 기본 통신 인터페이스로 유지하며, 새로운 제어 기능은 가능하면 이 인터페이스를 조합하여 구현한다.

프레임워크 내부 구현이 복잡하더라도, 일반적인 Application 개발자가 불필요한 내부 구현을 반복해서 다루지 않도록 하는 것을 지향한다.