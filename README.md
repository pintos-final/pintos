# 🔄 작업 흐름

### 1. 이슈 생성

```c
제목: [Project 1] Alarm Clock 구현
내용: 템플릿에 맞춰 작성
```

### 2. 브랜치 생성 및 체크아웃

```bash
git checkout develop
git pull origin develop
git checkout -b feat/alarm-clock
```

### 3. 작업 & 커밋

```bash
*# 작업 후 스테이징*
git add threads/thread.c devices/timer.c

*# 컨벤션에 맞게 커밋*
git commit -m "feat: sleep_list 자료구조 구현"
```

### 4. 푸시 & PR 생성

```bash
git push origin feat/alarm-clock
```

→ GitHub에서 PR 생성 (develop ← feat/alarm-clock)

### 5. 코드 리뷰 & 머지

- 팀원 리뷰 후 Approve
- Squash and Merge 또는 Merge commit

# 🌿 브랜치 전략

```
main (또는 master)
 └── develop
      ├── feat/alarm-clock
      ├── feat/priority-scheduling
      ├── feat/priority-donation
      └── fix/alarm-negative

```

### 브랜치 네이밍

| 타입 | 형식 | 예시 |
| --- | --- | --- |
| 기능 개발 | `gitname/project/기능명` | `hyun/threads/alarm-clock` |
| 버그 수정 | `fix/버그명`  | `fix/priority-inversion` |
| 리팩토링 | `refactor/대상` | `refactor/semaphore` |
| 문서 작업 | `docs/문서명` | `docs/readme` |

# 📋 이슈 & PR 연결

### 이슈 자동 종료 키워드

PR 본문에 아래 키워드 + 이슈번호 작성 시 머지되면 이슈 자동 종료:

- `closes #123`
    - **Close**는 단순히 이슈를 '닫는다'는 의미
    - 버그 수정이 아닌, 새로운 **기능 개발, 문서 업데이트, 리팩토링** 등 일반적인 작업이 끝났을 때 주로 사용
    - 예: "사용자 매뉴얼 문서 작성 완료", "불필요한 주석 제거"
- `fixes #123`
    - 주로 코드의 **버그나 오류를 수정**했을 때 사용
    - 예: "로그인 버튼이 작동하지 않는 버그 수정"
- `resolves #123`
    - 버그 수정보다는 **사용자의 요청, 문의, 또는 장기적인 과제**를 해결했을 때 적합
    - 가끔 여러 이슈를 동시에 해결할 때나, 버그/기능 구현이 섞여 있을 때 중립적인 표현으로 쓰기도 함

# 📘 프로젝트 커밋 컨벤션

## 📝 커밋 메시지 구조

```
<타입>: <설명>

[본문(선택)]
```

### 예시

```
feat: busy waiting 제거를 위한 sleep_list 구현

- timer_sleep()에서 busy waiting 대신 sleep_list를 사용하여
스레드를 블록시키고 timer_interrupt()에서 깨우도록 변경
```

## **🏷️ 타입 (Type)**

### **필수 타입**

| **타입** | **설명** | **예시** |
| --- | --- | --- |
| `feat` | 새로운 기능 추가 | `feat: priority scheduling 구현` |
| `fix` | 버그 수정 | `fix: alarm-negative 테스트 실패 수정` |

### **선택 타입**

| **타입** | **설명** | **예시** |
| --- | --- | --- |
| `docs` | 문서 수정 | `docs: README에 빌드 방법 추가` |
| `style`  | 코드 포맷팅, 세미콜론 누락 등 (기능 변경 X) | `style: 들여쓰기 수정` |
| `refactor` | 코드 리팩토링 (기능 변경 X) | `refactor: thread_yield 로직 개선` |
| `test` | 테스트 코드 추가/수정 | `test: priority-donate 테스트 추가` |
| `chore` | 빌드, 설정 파일 수정 등 | `chore: Makefile 경로 수정` |

## ✍️ 설명 (Subject) 규칙

1. **50자 이내**로 작성
2. **마침표(.) 금지**
3. **명령문** 형태로 작성 (과거형 X)
4. **첫 글자 소문자** (영어일 경우)
    
    ```
    *# ✅ 좋은 예*
    feat(alarm): sleep_list 자료구조 추가
    fix(donation): nested donation 깊이 제한 수정
    
    *# ❌ 나쁜 예*
    feat(alarm): sleep_list 자료구조 추가.     *# 마침표 X*
    feat(alarm): Added sleep_list              *# 과거형 X*
    feat(alarm): Sleep_list 자료구조 추가       *# 첫 글자 소문자*
    ```
    

## 📄 본문 (Body) 규칙

- 설명과 본문 사이에 **빈 줄** 필수
- **무엇을**, **왜** 변경했는지 설명
- **72자마다 줄바꿈** 권장
    
    ```
    feat(timer): busy waiting 제거
    
    기존 timer_sleep()은 while 루프로 busy waiting하여
    CPU 자원을 낭비했음. sleep_list를 도입하여 스레드를
    블록시키고 timer_interrupt()에서 깨우도록 변경.
    
    - thread 구조체에 wakeup_tick 필드 추가
    - sleep_list를 tick 기준 오름차순 정렬
    - timer_interrupt()에서 깨울 스레드 확인
    ```
    

---

## 📋 커밋 예시 모음

### 기능 구현

```bash
*# Alarm Clock*
git commit -m "feat(alarm): thread 구조체에 wakeup_tick 필드 추가"
git commit -m "feat(alarm): sleep_list 자료구조 구현"
git commit -m "feat(timer): timer_sleep()에서 busy waiting 제거"

*# Priority Scheduling*
git commit -m "feat(priority): ready_list 우선순위 정렬 구현"
git commit -m "feat(sync): sema_up()에서 우선순위 기반 스레드 선택"

*# Priority Donation*
git commit -m "feat(donation): lock 획득 시 priority donation 구현"
git commit -m "feat(donation): nested donation 지원"
git commit -m "feat(donation): multiple donation 지원"
```

### 버그 수정

```bash
git commit -m "fix(alarm): alarm-negative 테스트 음수 tick 처리"
git commit -m "fix(donation): priority-donate-chain 무한루프 수정"
git commit -m "fix(mlfqs): fixed-point 연산 오버플로우 수정"
```

### 리팩토링

```bash
git commit -m "refactor(thread): thread_yield() 로직 단순화"
git commit -m "refactor(sync): lock 관련 함수 분리"
```

### 문서

```bash
git commit -m "docs: README에 팀 정보 추가"
git commit -m "docs(alarm): 구현 방법 주석 추가"
```

### 기타

```bash
git commit -m "chore: .gitignore에 build 폴더 추가"
git commit -m "style(thread): 코드 포맷팅 통일"
git commit -m "test: alarm 관련 디버그 출력 추가"
```