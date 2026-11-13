# mini-rdbms Phase 1 구현 명세서

| 항목 | 내용 |
|---|---|
| 문서 대상 | Phase 1 (파일 기반 SQL 처리기) |
| 언어/표준 | C++17, 표준 라이브러리만 사용 (외부 DB·파서 라이브러리 없음) |
| 빌드 | CMake 3.16+ / `build.ps1` (Windows) |
| 소스 규모 | `sql.cpp` 218줄, `database.cpp` 342줄, `main.cpp` 82줄, `rdbms.hpp` 86줄 |

---

## 1. 목표와 범위

### 1.1 목표
- SQL 텍스트를 받아 파싱하고, 디스크 파일에 행을 저장/조회하는 최소 관계형 DB 엔진을 만든다.
- CLI와 엔진을 분리해, 이후 Phase(B+ 트리 인덱스, API 서버)에서 `Engine`을 그대로 재사용할 수 있게 한다.

### 1.2 지원 범위 (In Scope)
- `INSERT INTO ... VALUES ...` (다중 행, 컬럼 목록 선택)
- `SELECT ... FROM ... [WHERE ... AND ...]`
- 스키마 파일(`schemas/<table>.schema`) 기반 테이블 정의
- 바이너리 파일 저장(`data/<table>.mdb`), 순차 스캔 조회
- CSV 출력, 종료 코드, 위치 정보가 포함된 오류 메시지

### 1.3 제외 범위 (Out of Scope)
`CREATE/UPDATE/DELETE`, `NULL`, 기본값, 자동 ID, `UNIQUE`, `OR`, 괄호 조건, `JOIN`, 정렬, 집계, 인덱스, API 서버, 트랜잭션, WAL, 동시성 제어(잠금), 스키마 마이그레이션, 체크섬, fsync 보장.

> Phase 1의 `id`는 일반 INT 컬럼이다. 자동 발급과 유일성 제약은 없다. (Phase 2 범위)

---

## 2. 전체 구조

```text
CLI (--file / --execute / stdin)          src/main.cpp
  → Lexer   : 글자 → 토큰(위치 포함)       src/sql.cpp
  → Parser  : 토큰 → Insert/Select AST     src/sql.cpp
  → Engine  : 스키마 검사·컬럼 바인딩·WHERE   src/database.cpp
  → FileStore: 파일 append / 순차 scan      src/database.cpp
  → Result → formatResult → 출력
```

| 파일 | 책임 |
|---|---|
| `include/rdbms.hpp` | 공개 타입(AST, Schema, Result)과 `parse`, `loadSchema`, `FileStore`, `Engine`, `formatResult` 선언 |
| `src/sql.cpp` | `Lexer`, `Parser` (재귀 하강, lookahead 1) |
| `src/database.cpp` | 스키마 로딩/검증, 프레임·행 인코딩, `FileStore`, `Engine`, CSV 포맷 |
| `src/main.cpp` | 인자 해석, SQL 입력 확보, 파싱→실행→출력, 종료 코드 |
| `tests/tests.cpp` | 파서·스키마·실행기·저장 포맷 C++ 테스트 |
| `tests/cli.py` | 실제 프로세스를 띄우는 블랙박스 테스트 (표준 라이브러리만 사용) |
| `schemas/`, `examples/demo.sql` | 예제 스키마(`users`, `products`)와 데모 스크립트 |

계층 규칙: `FileStore`는 SQL/WHERE/출력 형식을 모르고, `main.cpp`는 SQL 의미를 모른다. 모든 심볼은 `namespace mini`.

---

## 3. 데이터 모델 (`rdbms.hpp`)

| 타입 | 정의 | 설명 |
|---|---|---|
| `Value` | `variant<int64_t, string>` | 한 칸의 값 |
| `Row` | `vector<Value>` | 스키마 컬럼 순서와 1:1 대응 |
| `Type` | `enum { Integer, Text }` | 컬럼 타입 (`INT`, `TEXT`) |
| `Column` | `{name, type}` | |
| `Schema` | `{table, columns}` | |
| `Compare` | `Equal, NotEqual, Less, LessEqual, Greater, GreaterEqual` | |
| `Condition` | `{column, op, value}` | WHERE 조건 1개 |
| `Insert` | `{table, columns, rows}` | `columns` 비어 있으면 스키마 순서 |
| `Select` | `{table, columns, conditions}` | `columns` 비어 있으면 `*`, 조건은 AND 결합 |
| `Statement` | `variant<Insert, Select>` | |
| `Result` | `{isSelect, inserted, columns, rows}` | |

공개 API:

```cpp
vector<Statement> parse(const string& sql);
Schema loadSchema(const filesystem::path& dir, const string& table);
class FileStore { append(schema, rows); scan(schema, visit) const; };
class Engine    { Engine(schemasDir, dataDir); Result execute(const Statement&); };
string formatResult(const Result&);
```

---

## 4. SQL 문법과 렉서/파서 (`sql.cpp`)

### 4.1 문법

```text
script   := (statement ';')* statement? ';'?
INSERT INTO table [ '(' column {',' column} ')' ] VALUES row {',' row}
row      := '(' literal {',' literal} ')'
SELECT ( '*' | column {',' column} ) FROM table
       [ WHERE cond { AND cond } ]
cond     := column operator literal
operator := = | != | <> | < | <= | > | >=
literal  := signed-64-bit-integer | 'text with ''escaped quotes'''
```

### 4.2 렉서 규칙
| 항목 | 규칙 |
|---|---|
| 토큰 종류 | `Word`, `Number`, `String`, `Symbol`, `End` |
| Word | `[A-Za-z_][A-Za-z0-9_]*`, **소문자로 정규화** (키워드·식별자 대소문자 무시) |
| Number | 숫자열, 또는 **바로 뒤에 숫자가 오는** `+`/`-` 접두 (`-5` 가능, `- 5` 불가) |
| String | `'...'`, 내부 `''` → `'`, 백슬래시는 특별 처리 없음. 문자열 안의 `;`, `--`, `/*`는 그대로 보존 |
| Symbol | `( ) , ; * = < > !` 만 허용. `<=`, `>=`, `!=`, `<>`는 한 토큰으로 결합 |
| 주석 | `-- 줄 끝까지`, `/* ... */` |
| BOM / CRLF | 입력 앞 UTF-8 BOM 건너뜀, CRLF 허용 |
| 위치 | 모든 토큰에 줄·열 기록 → 오류 `SQL <줄>:<열>: <메시지>` |

### 4.3 파서 규칙
- 입력 **전체를 먼저 파싱**한다. 구문 오류가 하나라도 있으면 어떤 문장도 실행되지 않는다.
- 빈 문장(`;;`) 허용, 마지막 `;` 생략 허용.
- 문장 뒤에는 `;` 또는 입력 끝만 올 수 있다. (예: `OR`는 `expected ';'` 오류)
- 정수는 `from_chars`로 변환하며 int64 범위를 벗어나면 `integer outside signed 64-bit range`.
- 오류 메시지: `expected INSERT or SELECT`, `expected 'values'`, `expected identifier`, `expected comparison operator`, `unterminated string`, `unterminated comment`, `unexpected character` 등.

---

## 5. 스키마 (`loadSchema`)

`schemas/<table>.schema` 파일이 테이블의 존재와 구조를 정의한다 (CREATE TABLE 없음).

```text
# 주석
id INT
name TEXT
age INT
```

| 규칙 | 내용 |
|---|---|
| 줄 형식 | `컬럼명 타입`, `#` 이후는 주석, 빈 줄 무시, 첫 줄 UTF-8 BOM 허용 |
| 타입 | `INT`(signed 64bit), `TEXT`(문자열) — 대문자 |
| 이름 | `[a-z_][a-z0-9_]*` (소문자). 테이블명은 파일명이 되므로 `../x` 같은 경로 조작을 차단 |
| 제약 | 컬럼 1~256개, 중복 이름 불가 |
| 오류 | 형식 위반 시 `Invalid schema at <경로>:<줄>`, 파일 없음 시 `Cannot open schema: ...` |

데이터 파일이 아직 없으면 **빈 테이블**로 취급하며 첫 INSERT에서 파일을 생성한다. 스키마는 `execute` 호출마다 다시 읽는다.

---

## 6. 저장 포맷 (`data/<table>.mdb`)

```text
8 bytes : "MRDB001\n"                      (매직)
frame   : 스키마 서명                        (테이블명 + 컬럼명/타입)
frame   : row 1
frame   : row 2 ...

frame       := u32 little-endian payload 길이 + payload
row payload := 각 컬럼마다 [u32 LE 값 길이][값 바이트]
```

- 스키마 서명 예: `"users\nid INT\nname TEXT\nage INT\n"`.
- INT는 **10진수 ASCII 문자열**, TEXT는 원본 UTF-8 바이트로 저장한다.
- 길이 접두 방식이므로 쉼표·개행·따옴표에 대한 이스케이프가 필요 없다.
- 길이는 비트 시프트로 직접 직렬화(엔디언 독립).
- 프레임/행 payload 상한 **16 MiB** (손상된 길이로 인한 거대 할당 방지).

### 6.1 읽기 시 검증 (손상 탐지)
| 상황 | 오류 메시지 |
|---|---|
| 매직 불일치 / 헤더 부족 | `Corrupt table: invalid format header` |
| 스키마 서명 불일치 | `Table schema does not match the stored schema` |
| 길이 4바이트가 일부만 존재 | `Corrupt table: incomplete frame length or read failure` |
| 길이 > 16 MiB | `Corrupt table: frame exceeds 16 MiB` |
| payload가 길이보다 짧음 | `Corrupt table: truncated frame or read failure` |
| 필드 길이/내용 부족 | `missing field length`, `truncated field` |
| INT 파싱 실패/범위 초과 | `Corrupt table: invalid integer` |
| 행에 남는 바이트 | `Corrupt table: extra bytes in row` |

체크섬은 없으므로 모든 비트 손상을 탐지하지는 못한다.

---

## 7. 실행 의미 (`Engine::execute`)

### 7.1 INSERT
1. 스키마 로드.
2. 컬럼 목록이 없으면 스키마 순서, 있으면 `mapping[i]` = 입력 i번째 값이 들어갈 스키마 위치.
3. 검증: 컬럼 중복 불가(`Duplicate INSERT column`), 존재하지 않는 컬럼 불가(`Unknown column`), **모든 컬럼을 정확히 한 번씩** 제공(`INSERT must supply every column; defaults and NULL are unsupported`), 행당 값 개수 일치(`INSERT value count does not match column count`), 타입 일치(`Type mismatch for column '...'`).
4. **모든 행을 메모리에서 먼저 검증·인코딩**한 뒤에만 파일을 연다 → 뒤쪽 행이 틀려도 앞쪽 행만 저장되는 일 없음.
5. `FileStore::append`: 기존 파일이면 헤더 확인 → `ios::app`로 끝에 추가 → `flush`/`close` 상태 확인. 새 파일이면 매직+서명 프레임부터 기록, `data/` 디렉터리 자동 생성.
6. 기존 레코드 전체를 재검사하지 않는다.

### 7.2 SELECT
1. 스키마 로드, 투영 컬럼(`*` 또는 목록)과 WHERE 컬럼을 **이름→인덱스로 사전 변환**.
2. WHERE 타입 검사는 데이터를 읽기 전에 수행 (`WHERE id='1'` → `Type mismatch`).
3. `FileStore::scan`으로 한 행씩 읽어 콜백에서 AND 단락 평가 → 통과한 행만 투영하여 결과에 추가. (파일 전체를 메모리에 올리지 않지만 결과는 메모리에 보관)
4. 정수는 수치 비교(`10 > 9`), 문자열은 UTF-8 바이트 순서 비교, 대소문자 구분.
5. 결과가 0행이어도 헤더는 반환.

### 7.3 문장/스크립트 수준
- 구문 오류 → 아무 것도 실행 안 함.
- 실행 중 오류 → 그 지점에서 중단, **앞서 성공한 문장은 유지** (스크립트 단위 트랜잭션 없음).

---

## 8. 출력과 CLI

### 8.1 출력 형식 (`formatResult`)
- INSERT: `INSERT <행 수>\n`
- SELECT: 헤더 + 데이터 행의 CSV. 필드에 `,` `"` `\r` `\n`이 있으면 `"..."`로 감싸고 내부 `"`는 `""`.
- 여러 문장은 실행 순서대로 이어서 출력.

### 8.2 CLI 인터페이스 (`main.cpp`)

```text
mini-rdbms [--schema-dir DIR] [--data-dir DIR] [--file FILE | --execute SQL]
           [--help | -h]
```

| 옵션 | 기본값 | 설명 |
|---|---|---|
| `--schema-dir` | `schemas` | 스키마 폴더 |
| `--data-dir` | `data` | 데이터 폴더 |
| `--file` | – | SQL 파일 (바이너리 모드로 읽음) |
| `--execute` | – | SQL 문자열 직접 전달 |
| (둘 다 없음) | – | 표준 입력을 EOF까지 읽어 배치 실행 (REPL 아님) |

- `--file`과 `--execute`는 동시/중복 사용 불가 (`Use only one --file or --execute option`).
- 알 수 없는 옵션, 값 누락, 파일 열기 실패는 오류.
- 경로는 실행 디렉터리 기준.
- 종료 코드: 성공 `0`, 실패 `1`. 오류는 **stderr**에 `error: ...`, 정상 결과는 stdout.

---

## 9. 빌드 구성

- `CMakeLists.txt`: 정적 라이브러리 `rdbms`(`sql.cpp`, `database.cpp`) + 실행 파일 `mini-rdbms`(`main.cpp`) + 테스트 `rdbms-tests`.
- 컴파일 옵션: MSVC `/W4 /utf-8 /permissive-`, 그 외 `-Wall -Wextra -Wpedantic`, `CXX_EXTENSIONS OFF`, MinGW는 정적 링크.
- CTest 등록: `unit-and-engine`(C++), `cli-persistence`(Python3 `tests/cli.py`). `-DBUILD_TESTING=OFF`로 테스트 제외 가능.
- `build.ps1`: PATH의 MinGW `clang++`/`g++` → `build-toolchain/*/bin/clang++.exe` → Visual Studio 순으로 컴파일러 탐색, `-Compiler`로 지정 가능, `-Test`로 테스트 실행. 산출물은 `build-native/`.

---

## 10. 테스트 전략

테스트는 임시 디렉터리에 데이터를 만들므로 실제 `data/`를 변경하지 않는다.

**C++ (`tests/tests.cpp`)** — 파서(문법·오류·주석·문자열), 스키마 검증(BOM, 잘못된 형식·중복), INSERT(컬럼 순서 재배치, 다중 행, 원자적 검증, 타입/개수/중복/미지 컬럼 오류), SELECT(6개 연산자, AND, 정수 수치 비교, 빈 결과 헤더), CSV 이스케이프, 테이블 간 격리, 스키마 변경 감지, 손상 탐지(잘림·길이 초과·헤더 불량), 10,000행 조회.

**CLI (`tests/cli.py`)** — 파일 입력(공백 경로, BOM), 프로세스 재시작 후 조회, 표준 입력, 구문 오류 시 미실행(`data/users.mdb` 미생성), 실행 오류 시 앞 문장 유지, 잘못된 인자 처리(`error:` + 종료 코드 1), `--help`, 주석만 있는 스크립트, 배포 데모(`examples/demo.sql`) 실행.

---

## 11. 알려진 한계

| 영역 | 한계 |
|---|---|
| 내구성 | fsync·WAL·롤백·자동 복구 없음. 쓰기 중 I/O 실패나 강제 종료에 대한 원자성 없음 (`Table write failed; partial data may exist`) |
| 동시성 | 단일 프로세스·단일 스레드. 같은 데이터 폴더의 다중 프로세스 사용 금지 |
| 성능 | SELECT는 항상 전체 순차 탐색. 결과·스크립트·INSERT 배치는 메모리 보관 (스트리밍 없음) |
| 스키마 | 변경은 마이그레이션 불가 (서명 불일치로 기존 데이터 거부) |
| 제약 | `id` 유일성·자동 발급 없음 |
| 기능 | 위 1.3 제외 범위 전체 |

## 12. Phase 2 확장 지점

- `Select.conditions`에서 id 동등 조건을 찾아 B+ 트리 검색 경로를 추가 (파일 오프셋과 인덱스 연결 필요).
- 자동 ID 발급 및 유일성 제약 → `Engine::execute`의 INSERT 경로.
- 100만 행 벤치마크는 Phase 1에 포함되지 않음.
- `Engine::execute(const Statement&)`가 CLI와 독립적이므로 API 서버에서 그대로 호출 가능.
