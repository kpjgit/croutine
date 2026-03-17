# croutine

> **C 기반 Stackless Coroutine Runtime**  
> 프레임 기반 상태 저장, 협력형 스케줄링, 타이머 대기, 그리고 `epoll` 기반 1단계 I/O 대기를 직접 구현하며  
> **코루틴의 내부 동작을 바닥부터 이해하기 위해 만든 학습형 런타임 프로젝트**

---

## 1. 프로젝트 소개

`croutine`은 C 언어로 작성한 **stackless coroutine runtime**입니다.  
이 프로젝트의 목적은 단순히 “코루틴처럼 보이는 문법”을 흉내 내는 데 있지 않습니다. 오히려 다음 질문에 직접 답해보는 데 가깝습니다.

- 스택을 바꾸지 않고 코루틴을 구현할 수 있을까?
- `yield` 이후 실행 지점은 어떻게 복원되는가?
- 지역 변수는 왜 사라지고, 무엇을 별도로 저장해야 하는가?
- 스케줄러는 `READY`, `WAITING`, `DONE` 상태를 어떻게 다루는가?
- 타이머 대기와 I/O 대기는 내부적으로 어떤 식으로 연결되는가?

이 프로젝트는 `setjmp/longjmp`, 어셈블리 컨텍스트 스위칭, 독립 스택 기반 구현을 사용하지 않고,  
**프레임(frame) + 상태 머신(pc) + 협력형 스케줄러**만으로 코루틴을 구성합니다.

즉, 이 저장소는 두 가지 목적을 동시에 가집니다.

- **실제로 실험 가능한 최소 코루틴 라이브러리**
- **코루틴/이벤트 루프/비동기 대기를 학습하기 위한 포트폴리오형 구현물**

---

## 2. 핵심 컨셉

이 프로젝트에서 말하는 **stackless coroutine**은 다음 의미에 가깝습니다.

- 코루틴마다 별도 스택을 두지 않음
- 레지스터 컨텍스트를 저장/복원하지 않음
- 실행 상태는 모두 **명시적인 프레임 객체**에 저장함
- 재개(resume)는 저장된 `pc(program counter)` 값을 통해 분기함

즉, 런타임이 숨겨서 처리하는 마법 같은 실행 컨텍스트가 있는 것이 아니라,  
코루틴의 상태가 **구조체로 드러나고**, 스케줄러가 그 구조체를 옮기며 실행을 이어가는 구조다.

이 설계를 통해 다음과 같은 학습 효과를 얻을 수 있다.

- 코루틴이 사실상 **상태 머신**임을 체감할 수 있음
- `yield`와 `await`가 내부적으로 어떤 의미인지 드러남
- 이벤트 루프와 코루틴이 어떻게 연결되는지 직접 확인 가능
- 비동기 프로그래밍의 “숨겨진 비용”과 “명시적 상태 관리”를 이해할 수 있음

---

## 3. 프로젝트 목표

### 학습 목표
- C에서 코루틴을 구현할 때 필요한 최소 요소를 구조적으로 이해
- 프레임 기반 continuation 모델 학습
- 협력형 스케줄러의 상태 전이 이해
- 타이머 대기와 I/O 대기의 내부 연결 방식 학습
- `epoll` 기반 readiness 모델과 코루틴의 결합 구조 이해

### 구현 목표
- 읽을 수 있을 정도로 작은 규모 유지
- 구현이 왜 그런 형태인지 코드상에서 드러나게 설계
- `yield -> timer wait -> io wait` 순서로 점진적 확장 가능하게 구성
- 실험 가능한 최소 런타임으로 유지

---

## 4. 디렉토리 / 파일 구성

```text
croutine.h          // 기본 프레임 구조, 상태, 매크로, 스케줄러 API, stage-1 I/O 대기 확장 헤더
croutine.c          // 기본 런타임 구현: ready queue + sleep list + timer wait, epoll 기반 I/O wait 구현
wait_test.c         // yield + timer wait 동작 확인용 예제
io_test.c           // nonblocking pipe 기반 reader/writer 예제
```

구성 자체도 학습 순서를 반영한다.

1. 먼저 코루틴 프레임과 `yield`를 이해한다.
2. 그다음 타이머를 붙이며 `WAITING` 상태를 다룬다.
3. 마지막으로 I/O 이벤트 루프와 연결해 실제 비동기 대기를 붙인다.

---

## 5. 전체 실행 구조

이 런타임의 실행 모델은 아래 한 문장으로 정리할 수 있다.

> **코루틴은 직접 기다리지 않는다.**  
> 기다리겠다는 의사를 프레임에 기록하고 `WAITING`을 반환한다.  
> 스케줄러가 그 프레임을 적절한 대기 자료구조로 이동시키고,  
> 조건이 만족되면 다시 `READY` 큐로 되돌린다.

즉, 코루틴 본문은 “계속 실행되는 함수”라기보다,
**한 번 조금 진행하고 상태를 반환하는 step 함수**로 동작한다.

실행 흐름은 대략 다음과 같다.

1. 스케줄러가 ready queue에서 프레임을 꺼낸다.
2. `step(frame, user_ctx)`를 호출한다.
3. 코루틴은 아래 셋 중 하나를 선택한다.
   - 계속 실행 가능 → `CO_ST_READY`
   - 무언가를 기다려야 함 → `CO_ST_WAITING`
   - 작업 종료/실패 → `CO_ST_DONE`, `CO_ST_ERROR`
4. 스케줄러는 반환값에 따라 프레임을 다시 큐에 넣거나 파괴한다.

---

## 6. 프레임 기반 설계

### 6.1 `co_frame_t`란 무엇인가

`co_frame_t`는 이 프로젝트의 핵심이다.  
하나의 코루틴은 결국 하나의 프레임 객체로 표현된다.

주요 필드는 다음과 같다.

- `pc`
  - 코루틴이 다음에 어디서 재개되어야 하는지 나타내는 값
- `status`
  - 현재 프레임이 READY / WAITING / DONE 중 어떤 상태인지 표현
- `err`
  - 오류 코드 저장용 공간
- `step`
  - 실제 코루틴 본문 함수 포인터
- `cleanup`
  - 프레임 종료 직전 호출되는 정리 함수
- `locals`
  - 코루틴 로컬 상태를 저장하는 사용자 영역
- `locals_size`
  - `locals` 크기 메타데이터
- `wait_kind`
  - 현재 어떤 종류의 대기인지 표현
- `wait`
  - 타이머 / I/O / future 확장을 위한 union
- `next`
  - intrusive linked list 연결 포인터
- `id`, `name`
  - 디버깅 및 관측용 정보
- `magic`
  - 잘못된 포인터 사용이나 이중 해제 탐지를 돕는 디버그 가드

### 6.2 왜 `locals`가 필요한가

일반적인 C 지역 변수는 함수가 리턴되면 스택에서 사라진다.  
그런데 코루틴은 `yield` 후 다시 이어서 실행되어야 한다.

즉, 아래와 같은 코드는 안전하지 않다.

```c
int i = 0; // yield 이후 유지되지 않음
```

대신 코루틴이 유지해야 할 상태는 모두 프레임 안에 넣어야 한다.

```c
my_locals_t* L = (my_locals_t*)fr->locals;
L->i = 0;
```

이 프로젝트에서 `locals`는 “코루틴 전용 힙 기반 로컬 스토리지”에 가깝다.

### 6.3 왜 `locals_size`도 기록하는가

현재 예제에서 직접 자주 쓰지는 않지만, 이 값은 다음 확장을 위한 중요한 메타데이터다.

- 잘못된 캐스팅/크기 사용을 점검하기 위한 디버그 정보
- 프레임 + 로컬을 한 번에 할당하는 구조로 바꿀 때 필요한 정보
- 프레임 종류별 메모리 사용량을 관측할 때 필요한 정보

즉, 단순한 포인터만 두는 것보다 “프레임이 이 로컬 저장소를 소유한다”는 사실을 더 명확하게 표현해준다.

### 6.4 왜 `next`가 프레임 안에 들어가는가

스케줄러는 ready queue, sleep list 같은 자료구조를 사용한다.  
보통은 별도 노드 구조체를 둘 수도 있지만, 이 프로젝트는 **intrusive linked list** 방식을 쓴다.

즉, 프레임 자체가 리스트 노드 역할도 겸한다.

장점:
- 추가 노드 할당이 필요 없음
- 소유권 구조가 단순해짐
- 캐시 친화적임

대신 한 프레임은 동시에 여러 리스트에 들어갈 수 없기 때문에,
언제 ready queue에 있고 언제 sleep list나 io wait registry로 이동하는지 흐름을 명확히 지켜야 한다.

---

## 7. 상태와 대기 종류

### 7.1 `co_status_t`

코루틴의 생명주기 상태를 표현한다.

- `CO_ST_READY`
  - 지금 바로 실행 가능한 상태
- `CO_ST_WAITING`
  - 타이머나 I/O 같은 외부 조건을 기다리는 상태
- `CO_ST_DONE`
  - 정상 종료
- `CO_ST_ERROR`
  - 오류로 종료
- `CO_ST_CANCELLED`
  - 취소됨

핵심은 `step()`이 이 상태를 반환하고,  
스케줄러는 이 값을 해석해서 프레임을 어디로 보낼지 결정한다는 점이다.

### 7.2 `co_wait_kind_t`

`WAITING` 상태가 되었을 때, **무엇을 기다리는지**를 표현한다.

- `CO_WAIT_NONE`
- `CO_WAIT_TIMER`
- `CO_WAIT_IO`
- `CO_WAIT_FUTURE`

즉 `status`는 “지금 기다리는 중이다”를 말하고,  
`wait_kind`는 “무엇을 기다리는 중인지”를 말한다.

### 7.3 왜 `wait`가 union인가

타이머 대기와 I/O 대기는 동시에 성립하지 않는다.  
따라서 대기 정보 저장 공간은 상호배타적이다.

그래서 다음과 같이 union을 사용했다.

- timer면 `wake_ms`
- io면 `fd`, `events`, `revents`
- future면 별도 포인터

이는 단순한 메모리 절약 이상의 의미가 있다.

- 프레임 구조가 명확해짐
- 대기 상태를 하나의 슬롯으로 관리하게 됨
- “한 시점에 하나의 대기 이유만 가진다”는 설계가 드러남

---

## 8. 스케줄러 구조

### 8.1 ready queue

즉시 실행 가능한 프레임을 담는 FIFO 큐다.

- `ready_head`
- `ready_tail`

`CO_YIELD` 이후 다시 READY가 된 프레임은 이 큐 뒤로 들어간다.  
즉, 가장 단순한 협력형 라운드로빈 스케줄링 형태라고 볼 수 있다.

### 8.2 sleep list

타이머를 기다리는 프레임은 `wake_ms` 기준으로 정렬된 연결 리스트에 저장된다.

- 가장 빨리 깨어나야 할 프레임이 항상 head에 위치
- 타이머를 깨울 때는 head만 반복적으로 확인하면 됨

삽입은 `O(n)`이지만, 구현이 단순하고 구조를 이해하기 쉽다.  
학습 목적의 런타임에서는 충분히 합리적인 선택이다.

### 8.3 time source와 `now_ctx`

스케줄러는 시간을 직접 하드코딩하지 않고, 콜백 함수 형태로 받는다.

```c
uint64_t (*co_now_ms_fn)(void* now_ctx)
```

이 구조는 단순히 플랫폼 종속성을 줄이기 위한 것만은 아니다.

`now_ctx`가 있으면:
- 테스트용 fake clock 주입
- 래핑된 시간 소스 사용
- 특정 플랫폼 타이머 상태 전달

같은 것이 가능하다.

즉, 시간 함수도 일종의 **주입 가능한 런타임 의존성**으로 본 것이다.

---

## 9. 매크로 기반 코루틴 변환

이 프로젝트는 protothreads 스타일 매크로를 사용한다.  
즉, 평범한 C 함수를 **재개 가능한 상태 머신**으로 바꾸는 방식이다.

### `CO_BEGIN(fr)`
`switch (fr->pc)` 구조를 시작한다.

### `CO_YIELD(fr)`
현재 라인 번호를 `pc`에 저장하고 `CO_ST_READY`를 반환한다.  
다음 실행 때는 저장된 지점으로 점프한다.

### `CO_AWAIT_UNTIL(fr, cond)`
조건이 만족될 때까지 `CO_ST_WAITING`을 반환한다.  
다음 실행에서도 같은 지점으로 돌아와 조건을 다시 검사한다.

### `CO_END(fr)`
정상 종료를 의미하며 `CO_ST_DONE`을 반환한다.

핵심은 이 매크로들이 “코루틴처럼 보이게” 만드는 것이 아니라,  
실제로는 **명시적인 program counter 기반 상태 머신**을 생성한다는 점이다.

---

## 10. 기본 런타임 로직

### 10.1 프레임 생성과 소멸

- `co_frame_create()`
  - 프레임 메타데이터 초기화
  - `locals` 메모리 할당
  - step/cleanup/name 등록
- `co_frame_destroy()`
  - cleanup 호출
  - locals 해제
  - 프레임 해제

즉, 프레임은 단순한 상태 저장소가 아니라,
**코루틴 실행 단위를 소유하는 객체**로 다뤄진다.

### 10.2 스케줄러 등록

- `co_spawn()`
  - 프레임에 ID를 부여하고 ready queue에 넣는다.

즉, 프레임을 만들었다고 자동으로 실행되는 것이 아니라,  
스케줄러에 등록되어야 runnable 상태가 된다.

### 10.3 타이머 대기

- `sleep_insert_sorted()`
  - wake 시각 기준으로 sleep list에 삽입
- `wake_due_timers()`
  - 현재 시각을 기준으로 깨워야 할 프레임을 ready queue로 이동

즉, `WAITING + TIMER` 상태는 결국 ready queue로 복귀할 경로를 가진다.

### 10.4 `co_sched_pump()`

이 함수가 런타임의 중심이다.

동작 흐름:
1. 만료된 타이머를 깨운다.
2. ready queue에서 프레임을 하나씩 꺼낸다.
3. `step(frame, user_ctx)`를 실행한다.
4. 반환 상태에 따라 분기한다.
   - READY → ready queue 재삽입
   - WAITING → timer list 또는 io wait registry로 이동
   - DONE / ERROR / CANCELLED → destroy

즉, pump는 단순 반복문이 아니라
**프레임 상태 전이를 실제로 수행하는 디스패처**다.

---

## 11. `user_ctx`의 의미

`step(frame, user_ctx)` 형태를 보면 처음에는 `locals`와 비슷해 보일 수 있다.  
하지만 역할은 완전히 다르다.

- `fr->locals`
  - 코루틴별 전용 상태
- `user_ctx`
  - 여러 코루틴이 공유하는 런타임 자원

예를 들어 `io_test.c`에서는 다음 정보를 `user_ctx`에 담았다.

- 읽기용 fd
- 쓰기용 fd

즉, `user_ctx`는 “전역 변수”를 직접 쓰는 대신,  
코루틴들이 공통으로 참조할 환경을 명시적으로 주입하는 방식이다.

이 구조는 다음 장점이 있다.

- 테스트가 쉬움
- 의존성이 명확함
- 런타임/애플리케이션 경계를 분리하기 좋음

---

## 12. Stage 1: epoll 기반 I/O 대기 확장

타이머 대기만 있으면 “시간이 지나면 다시 실행”만 가능하다.  
하지만 실제 비동기 시스템에서는 대부분 I/O readiness를 기다려야 한다.

그래서 stage 1에서는 Linux `epoll`을 사용해 **I/O 대기 레이어**를 추가했다.

### 12.1 추가된 핵심 요소

#### 내부 이벤트 비트
- `CO_IO_READ`
- `CO_IO_WRITE`
- `CO_IO_ERR`
- `CO_IO_HUP`

코루틴 코드가 `EPOLLIN` 같은 OS 상수에 직접 의존하지 않도록,
내부 이벤트 표현을 별도로 정의했다.

#### I/O wait payload

`wait.io`는 다음 필드를 가진다.

- `fd`
- `events`
- `revents`

의미는 다음과 같다.

- `events`: 내가 무엇을 기다리는가
- `revents`: 실제로 어떤 이벤트가 와서 깨어났는가

#### scheduler 확장

`co_sched_t`에 다음 필드가 추가되었다.

- `epfd`
- `fd_waiters`
- `fd_waiters_cap`
- `io_waiter_count`

즉, 이제 스케줄러는 ready/sleep뿐 아니라 **I/O wait registry**도 관리한다.

### 12.2 fd당 waiter 제약

1단계 구현에서는 구조를 단순하게 유지하기 위해 다음 제약을 둔다.

- 한 fd당 read waiter 1개
- 한 fd당 write waiter 1개

이 제약 덕분에 자료구조가 단순해지고,
처음 구현에서 발생할 수 있는 fairness나 thundering herd 문제를 피할 수 있다.

### 12.3 I/O 대기 흐름

코루틴이 `CO_WAIT_READ(fr, fd)`를 호출하면:

1. 프레임에 `WAITING + IO` 정보가 기록된다.
2. pump가 이 프레임을 `register_io_wait` 계열 로직으로 등록한다.
3. 해당 fd는 `epoll` 감시 대상으로 들어간다.
4. `epoll_wait()`가 이벤트를 반환하면 스케줄러가 프레임을 ready queue로 옮긴다.
5. 다음 step에서 코루틴은 같은 지점부터 재개된다.

즉,
**코루틴은 직접 잠들지 않고, 스케줄러가 대신 OS 이벤트 루프와 연결해준다.**

### 12.4 왜 이벤트가 왔다고 바로 read/write하지 않는가

I/O readiness는 “지금 시도해볼 수 있다”는 뜻이지,
항상 성공을 보장하는 것은 아니다.

그래서 코루틴 본문은 항상 다음 패턴을 따른다.

1. 먼저 `read()` / `write()`를 시도
2. `EAGAIN`이면 `CO_WAIT_READ` / `CO_WAIT_WRITE`
3. 깨어나면 다시 시스템콜 시도

이 패턴은 비동기 I/O의 핵심 습관이다.

---

## 13. 예제 설명

### 13.1 `wait_test.c`

기본 예제는 다음을 보여준다.

- `CO_YIELD`로 협력형 양보
- `CO_WAIT_TIMER`와 `CO_AWAIT_UNTIL`로 시간 대기
- `fr->locals`를 통한 상태 유지

즉, “스택 없이도 코루틴처럼 이어서 실행된다”는 점을 가장 단순하게 확인할 수 있다.

### 13.2 `io_test.c`

I/O 예제는 nonblocking pipe를 사용해 다음 흐름을 보여준다.

- reader 코루틴
  - 먼저 `read()` 시도
  - 막혀 있으면 `CO_WAIT_READ`
  - 깨어난 뒤 다시 `read()` 시도

- writer 코루틴
  - 500ms 타이머 대기
  - `write()` 시도
  - 필요하면 `CO_WAIT_WRITE`

이 예제는 타이머 대기와 I/O 대기가 모두 동일한 pump 구조 안에서 처리된다는 점을 보여준다.

---

## 14. 사용 방법

### 기본 타이머 예제 실행

```bash
cc -O2 -Wall -Wextra -std=c11 wait_test.c croutine.c -o wait_test
./wait_test
```

### stage-1 I/O 예제 실행

```bash
cc -O2 -Wall -Wextra -std=c11 io_test.c croutine.c -o io_test
./io_test
```

### 코루틴 작성 기본 패턴

```c
static co_status_t my_step(co_frame_t* fr, void* user_ctx) {
    my_locals_t* L = (my_locals_t*)fr->locals;

    CO_BEGIN(fr);

    // ... 작업 ...
    CO_YIELD(fr);

    // ... 조건 대기 ...
    CO_AWAIT_UNTIL(fr, some_condition);

    CO_END(fr);
}
```

핵심 규칙은 간단하다.

- `yield` 이후에도 유지되어야 하는 상태는 `fr->locals`에 넣는다.
- 외부 공유 자원은 `user_ctx`로 전달한다.
- 대기가 필요하면 직접 블로킹하지 말고 `WAITING` 상태를 반환한다.

---

## 15. 이 프로젝트에서 특히 강조한 설계 포인트

### 15.1 “보이는 코루틴”이 아니라 “이해되는 코루틴”

이 프로젝트는 추상화를 과하게 감추지 않았다.  
왜냐하면 목적이 완제품 라이브러리보다 **구조 이해**에 더 가깝기 때문이다.

그래서 다음이 코드에 그대로 드러난다.

- 프레임이 어디에 저장되는지
- 스케줄러가 프레임을 어떤 큐로 옮기는지
- `yield`가 사실상 `pc 저장 + return`이라는 점
- `await`가 사실상 “조건 충족 전까지 WAITING 반환”이라는 점

### 15.2 작은 구현으로 continuation 개념을 체감

“코루틴은 continuation을 저장한다”는 말을 이론으로만 아는 것과,  
실제로 `pc`, `locals`, `wait_kind`를 구조체에 넣어보고 스케줄러로 움직여보는 것은 완전히 다르다.

이 프로젝트는 그 차이를 체감하기 위한 구현이다.

### 15.3 타이머와 I/O를 같은 상태 전이 모델로 통합

이 프로젝트에서 타이머와 I/O는 겉보기엔 다른 기능이지만,
런타임 내부에서는 같은 패턴으로 취급된다.

- 코루틴이 `WAITING`을 반환
- 스케줄러가 적절한 대기 자료구조에 등록
- 조건 만족 시 READY로 복귀

이 통합된 시각은 이후 더 큰 이벤트 루프나 async runtime을 이해하는 데 중요한 기반이 된다.

---

## 16. 한계와 확장 방향

현재 stage 1 구현은 의도적으로 단순화되어 있다.

### 현재 한계
- Linux `epoll` 전용
- fd당 read waiter 1개 / write waiter 1개
- cancellation이 I/O registry에서 즉시 제거되지는 않음
- I/O timeout이 별도 통합되어 있지 않음
- close/hup 처리 정책이 최소 수준임

### 다음 확장 방향
- cancel 시 I/O waiter 즉시 제거
- `connect`, `accept`, `read`, `write` 헬퍼 함수 추가
- I/O timeout 통합
- sleep list를 min-heap으로 전환
- `frame + locals` 일체형 할당 구조 적용
- 멀티스레드 환경을 위한 구조 확장

즉, 현재 구현은 끝난 라이브러리가 아니라,
**기초 모델을 명확히 세운 뒤 단계적으로 확장 가능한 토대**에 가깝다.

---

## 17. 회고

이 프로젝트를 통해 가장 크게 확인한 점은 다음과 같다.

- 코루틴은 문법이 아니라 **상태 저장 방식**이다.
- `yield`와 `await`는 특별한 마법이 아니라 **명시적인 제어권 반환**이다.
- 비동기 런타임의 본질은 “코드를 동시에 실행하는 것”보다,  
  **기다리는 작업을 적절한 자료구조에 등록하고 다시 깨우는 것**에 있다.
- C처럼 저수준 언어에서 직접 구현해보면, 고수준 async/await의 내부 모델이 훨씬 선명하게 보인다.

이 저장소는 단순히 “코루틴을 하나 만들었다”는 결과물보다,
**코루틴 런타임을 직접 해부하고 재구성한 학습 기록**으로 의미가 있다.

---

## 18. 빌드 환경

- Language: C11
- Platform:
  - 기본 런타임: POSIX 계열 환경
  - stage-1 I/O: Linux (`epoll` 사용)
- Compiler: `cc` 또는 `gcc/clang`

---

## 19. 마무리

- continuation
- cooperative scheduling
- explicit state machine
- timer wait
- nonblocking I/O
- event loop integration

