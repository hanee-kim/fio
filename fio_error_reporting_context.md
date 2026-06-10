# fio 에러 리포팅 분석 컨텍스트

이 문서는 fio(Flexible I/O Tester)의 에러 리포팅 메커니즘을 분석한 내용입니다.
AI 어시스턴트에게 fio 에러 관련 질문을 할 때 이 문서를 컨텍스트로 제공하세요.

---

## 1. 에러 저장 구조

fio는 각 job(스레드)마다 `struct thread_data`에 에러를 저장합니다.

```c
// fio.h:254-255
int error;                    // errno 값 (e.g. EIO=5, ENOSPC=28)
char verror[128];             // 상세 메시지: "file:io_u.c:2021, func=io_u error, error=Input/output error"
```

에러 설정은 `td_verror()` 매크로로 이루어집니다 (`fio.h:558`):
```c
td_verror(td, errno, "함수명");
// → td->error = errno
// → td->verror = "file:<파일>:<줄>, func=<함수명>, error=<strerror(errno)>"
```

---

## 2. I/O 에러 전파 경로

```
엔진 read/write 실패
  → io_u->error = errno                        (io_u.c)
  → __io_u_log_error()                         (io_u.c:1988)
      ├─ log_err() → stderr 출력               파일명, offset, strerror 포함
      └─ td_verror() → td->error 설정
  → 스레드 종료
      └─ log_info("fio: pid=%d, err=%d/%s")    (backend.c:2204-2206) → stdout 출력
  → exit_value++                               (backend.c:2345-2346)  실패 job당 +1
  → main() return exit_value
```

핵심: **exit code는 errno가 아닌 실패한 job의 수입니다.**

---

## 3. exit code 의미

| exit code | 의미 |
|---|---|
| 0 | 전체 성공 |
| N (양수) | N개의 job이 에러로 종료 |
| 128 | SIGINT/SIGTERM으로 종료 (backend.c:94) |

---

## 4. 출력 채널별 에러 정보

| 채널 | 내용 | 시점 |
|---|---|---|
| **stderr** | `fio: io_u error on file <파일>: <strerror>: <ddir> offset=<N>, buflen=<N>` | I/O 에러 발생 즉시 |
| **stdout** | `fio: pid=<PID>, err=<errno>/<verror>` | job 종료 시 |
| **JSON** | `job["error"]`, `job["total_err"]`, `job["first_error"]` | 전체 완료 후 |

---

## 5. 셋업 단계 에러 커버리지

| 에러 종류 | `td->error` 설정 | 출력 경로 |
|---|---|---|
| 파일 open/create/stat 실패 (`filesetup.c`) | **O** (`td_verror` 사용) | stderr + stdout (pid/err/verror) |
| `open state file` 실패 (`verify.c:1733`) | **X** | `perror()` → stderr만 |
| bad option / 옵션 검증 실패 (`init.c`) | **대부분 X** | `log_err()` → stderr만 |

> **주의:** `open state file` 실패 시 `perror()`로 errno가 stderr에 출력되지만,
> `td->error`가 설정되지 않아 JSON 출력이나 구조화된 리포팅에는 포함되지 않습니다.

---

## 6. Python으로 에러 얻기

### 기본 패턴 (JSON + 파일 분리)

normal 출력은 그대로 유지하면서 JSON을 별도 파일로 저장합니다.

```python
import subprocess
import json

def run_fio(job_file, json_output="/tmp/fio_result.json"):
    result = subprocess.run(
        [
            "fio",
            "--output-format=normal,json",   # normal 출력 유지 + JSON 병행
            f"--output={json_output}",        # JSON을 파일에 저장
            job_file,
        ],
        text=True,
    )

    # exit code = 실패한 job 수
    if result.returncode != 0:
        # stderr: io_u 에러 상세 (파일명, offset, strerror)
        print("[stderr]", result.stderr)

        # JSON에서 errno 파싱
        with open(json_output) as f:
            data = json.load(f)
        for job in data.get("jobs", []):
            err = job.get("error", 0)
            if err:
                print(f"job '{job['jobname']}': errno={err}")
            if job.get("total_err"):
                print(f"  total_err={job['total_err']}, first_error={job['first_error']}")

    return result.returncode
```

### JSON에서 꺼낼 수 있는 에러 필드

```python
data = json.load(open("fio_result.json"))
job  = data["jobs"][0]

job["error"]        # errno 값 (0이면 성공)
job["total_err"]    # 총 에러 횟수 (continue_on_error 옵션 사용 시)
job["first_error"]  # 첫 번째 에러 errno
```

`total_err` / `first_error`는 fio 옵션에 `continue_on_error=all`을 설정해야 JSON에 포함됩니다.

### stderr에서 I/O 에러 상세 파싱

```python
import re

io_errors = re.findall(
    r"fio: io_u error(?:\s+on file\s+(\S+))?: (.+?): (\w+) offset=(\d+), buflen=(\d+)",
    result.stderr,
)
for filename, err_msg, direction, offset, buflen in io_errors:
    print(f"file={filename}, error={err_msg}, dir={direction}, offset={offset}")
```

---

## 7. 알려진 한계

- **exit code로 errno 직접 획득 불가**: exit code는 실패 job 수이므로 `$?`로 errno를 얻을 수 없습니다.
- **셋업 에러의 JSON 미포함**: `open state file` 실패, 일부 옵션 검증 오류는 `td->error`가 설정되지 않아 JSON `error` 필드에 나타나지 않습니다. 이 경우 stderr를 파싱해야 합니다.
- **verror 크기 제한**: `td->verror`는 128바이트로 제한됩니다 (`stat.h:147`).

---

## 8. 관련 소스 위치

| 파일 | 내용 |
|---|---|
| `fio.h:254-255, 558-568` | `td->error`, `td->verror`, `td_verror()` 매크로 정의 |
| `io_u.c:1988-2022` | `__io_u_log_error()` — I/O 에러 로깅 + td_verror 호출 |
| `backend.c:2204-2206` | job 종료 시 errno + verror stdout 출력 |
| `backend.c:2345-2346` | exit_value 누적 |
| `backend.c:2563-2568` | setup_files 실패 시 errno + verror 출력 |
| `filesetup.c` | 파일 셋업 에러 — td_verror 광범위하게 사용 |
| `verify.c:1731-1735` | open state file 실패 — perror만 사용, td_verror 미사용 |
| `init.c` | 옵션 검증 — 대부분 log_err만 사용 |
| `stat.c:1773, 1892` | JSON error 필드 출력 |
| `t/fiotestlib.py` | fio 공식 Python 테스트 라이브러리 (subprocess + JSON 파싱) |
