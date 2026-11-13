# mini-rdbms — Phase 1

C++17 표준 라이브러리로 구현한 파일 기반 SQL 처리기입니다. 외부 DB나 SQL 파서 라이브러리를 사용하지 않습니다. CLI와 엔진을 분리해 이후 B+ 트리와 API 서버에서 엔진을 재사용할 수 있게 구성했습니다.

## 빠른 실행 (Windows PowerShell)

PowerShell에서:

```powershell
cd C:\Github\mini-rdbms
.\build.ps1 -Test
.\build-native\mini-rdbms.exe --file examples/demo.sql
.\build-native\mini-rdbms.exe --execute "SELECT name, age FROM users WHERE age >= 25;"
```

`build.ps1`은 PATH의 MinGW `clang++`/`g++`, `build-toolchain/*/bin/clang++.exe`, Visual Studio C++ 도구 순으로 찾습니다. Visual Studio에는 **Desktop development with C++ 및 Windows SDK**가 필요합니다. 컴파일러를 지정할 수도 있습니다.

```powershell
.\build.ps1 -Compiler C:\toolchains\llvm-mingw\bin\clang++.exe -Test
```

`-Test`는 C++ 테스트와 Python 3 표준 라이브러리 기반 CLI 테스트를 실행합니다. DB 실행에는 Python이 필요 없습니다. `build-native`의 실행 파일은 저장소에서 추적하지 않습니다. 기본 데이터는 현재 작업 디렉터리의 `data/`에 저장되므로 같은 예제를 다시 실행하면 행이 추가됩니다.

## CMake 빌드 (Windows / Linux / macOS)

CMake 3.16+, C++17 컴파일러, 테스트용 Python 3이 필요합니다.

```sh
cd mini-rdbms
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
./build/mini-rdbms --file examples/demo.sql
```

Visual Studio 같은 다중 구성 생성기에서는 실행 파일이 `build/Release/mini-rdbms.exe`에 있습니다. 테스트를 빼고 빌드하려면 `-DBUILD_TESTING=OFF`를 지정하면 됩니다. 오래된 CTest에서 `--test-dir`을 지원하지 않으면 `build` 폴더에서 `ctest -C Release --output-on-failure`를 실행합니다.

## 입력과 출력

```sh
# SQL 텍스트 파일
mini-rdbms --schema-dir schemas --data-dir data --file examples/demo.sql

# 직접 전달
mini-rdbms --execute "SELECT * FROM users WHERE id = 2;"

# 표준 입력 (EOF까지 읽은 뒤 실행)
echo "SELECT * FROM users;" | mini-rdbms

# 도움말
mini-rdbms --help
```

경로는 실행한 디렉터리를 기준으로 해석합니다. `--schema-dir`, `--data-dir`의 기본값은 각각 `schemas`, `data`입니다. `--file`과 `--execute`는 함께 쓸 수 없습니다. 표준 입력은 대화형 REPL이 아닌 배치 입력입니다.

- INSERT 성공: `INSERT 3`처럼 삽입한 행 수를 출력합니다.
- SELECT 성공: 컬럼 헤더를 포함한 CSV를 출력합니다. 쉼표·쌍따옴표·개행이 포함된 필드는 CSV 인용 규칙으로 출력합니다. 결과가 없어도 헤더는 출력합니다.
- 실패: 표준 오류에 `error: ...`를 출력하고 종료 코드 `1`을 반환합니다. 성공은 `0`입니다.
- 하나의 파일에 여러 문장이 있으면 결과를 순서대로 출력합니다. 전체 출력에는 INSERT 메시지와 여러 SELECT 결과가 섞일 수 있으므로, CSV 내보내기는 SELECT 한 문장으로 실행하세요.
- SQL과 스키마 파일은 UTF-8을 사용합니다. BOM, CRLF를 허용합니다. 터미널 한글 출력이 깨지면 UTF-8 터미널을 사용하세요.

## 지원하는 SQL

```sql
INSERT INTO users VALUES (1, '김정글', 25);
INSERT INTO users (name, age, id) VALUES ('O''Brien', 31, 2), ('Alice', 22, 3);

SELECT * FROM users;
SELECT name, age FROM users WHERE id = 2;
SELECT id FROM users WHERE age >= 20 AND age < 30;
SELECT name FROM users WHERE name != 'Alice';
```

```text
script := (statement ';')* statement? ';'?
INSERT INTO table [(column [, column ...])] VALUES (literal, ...) [, (literal, ...)]
SELECT ('*' | column [, column ...]) FROM table
       [WHERE column operator literal [AND column operator literal ...]]
operator := = | != | <> | < | <= | > | >=
literal := signed-64-bit-integer | 'text with ''escaped quotes'''
```

SQL 키워드와 식별자는 대소문자를 구분하지 않으며 내부에서 소문자로 정규화합니다. 식별자는 `[A-Za-z_][A-Za-z0-9_]*`입니다. 문자열은 대소문자를 구분하고 UTF-8 바이트 순서로 비교합니다. 정수는 수치로 비교합니다.

여러 문장은 세미콜론으로 구분하며 마지막 세미콜론은 생략할 수 있습니다. `-- 한 줄 주석`, `/* 여러 줄 주석 */`을 지원합니다. 문자열 안의 세미콜론이나 주석 기호는 그대로 보존합니다. 작은따옴표는 `''`로 이스케이프하며, 백슬래시는 특별하게 해석하지 않습니다.

컬럼 목록을 지정해도 **모든 컬럼을 정확히 한 번씩** 제공해야 합니다. 컬럼 목록을 생략하면 스키마 순서입니다. INSERT의 행 개수·타입을 모두 검증한 뒤 파일에 추가하므로, 뒤쪽 행에 잘못된 값이 있어도 해당 INSERT의 앞쪽 행만 저장되는 일은 없습니다. 단, 쓰기 중 I/O 실패나 프로세스 강제 종료에 대한 원자성은 제공하지 않습니다.

## 미리 정의된 스키마

CREATE TABLE 없이 `schemas/<table>.schema` 파일이 테이블의 존재와 구조를 정의합니다. 데이터 파일이 아직 없으면 빈 테이블로 조회되며 첫 INSERT에서 데이터 파일을 생성합니다.

`schemas/users.schema`:

```text
# 컬럼명 타입
id INT
name TEXT
age INT
```

`INT`는 부호 있는 64비트 정수, `TEXT`는 문자열입니다. 스키마 파일의 컬럼명은 소문자 식별자, 타입은 `INT`/`TEXT` 대문자를 사용합니다. 한 테이블은 1~256개 컬럼을 가질 수 있으며 중복 이름을 허용하지 않습니다. `#` 뒤는 주석입니다. `products` 스키마도 예제로 제공합니다.

Phase 1의 `id`는 일반 INT 컬럼입니다. 사용자가 직접 값을 넣으며 **자동 발급과 유일성 제약은 없습니다**. 자동 ID와 B+ 트리 인덱스는 Phase 2 범위입니다.

## 처리 흐름과 코드

```text
CLI (--file / --execute / stdin)
  → Lexer: 토큰과 위치 추출
  → Parser: Insert / Select AST 생성
  → Engine: 스키마 검사, 컬럼 바인딩, 조건 평가
  → FileStore: 테이블 파일 append / 순차 scan
  → Result → CSV 출력
```

| 파일 | 책임 |
|---|---|
| `include/rdbms.hpp` | AST, 스키마, 결과, 엔진·저장소의 공개 인터페이스 |
| `src/sql.cpp` | 문자열·주석을 구분하는 렉서와 재귀 하강 파서 |
| `src/database.cpp` | 스키마 로딩, 바이너리 직렬화, 실행, CSV 포맷팅 |
| `src/main.cpp` | CLI 옵션, SQL 입력, 출력과 종료 코드 |
| `tests/tests.cpp` | 파서·스키마·실행기·저장 포맷 테스트 |
| `tests/cli.py` | 실제 프로세스 실행, 재시작, 파일·표준 입력 테스트 |

`Engine::execute(const Statement&)`는 CLI와 독립적으로 호출할 수 있습니다. SELECT는 현재 모든 레코드를 순차 탐색합니다. 이후 `Select.conditions`에서 ID 동등 조건을 찾고 검색 경로를 추가할 수 있으며, 그때 파일 위치와 인덱스의 연결도 구현해야 합니다.

## 저장 포맷

테이블별로 `data/users.mdb`, `data/products.mdb`를 사용합니다.

```text
8 bytes: MRDB001\n
frame: 테이블명 + 정규화된 컬럼명/타입 (스키마 서명)
frame: row 1
frame: row 2
...

frame := 4-byte little-endian payload length + payload
row payload := 각 컬럼의 [4-byte byte length + value bytes]
```

정수 값은 10진수 ASCII, TEXT는 원래 문자열 바이트로 저장합니다. 필드 길이를 명시하므로 쉼표, 개행, 따옴표를 저장 구분자와 혼동하지 않습니다. 한 행의 payload 상한은 16 MiB입니다. 스키마 서명을 확인해 컬럼 변경 후 기존 데이터를 잘못 해석하는 것을 막습니다. 스키마 변경은 마이그레이션 기능이 아니므로 기존 데이터 파일에 그대로 적용할 수 없습니다.

읽을 때 잘못된 헤더, 스키마 불일치, 잘린 길이/레코드, 과도한 길이, 잘못된 정수 등을 오류로 보고합니다. 체크섬은 없으므로 모든 비트 손상을 탐지하지는 못합니다. INSERT는 헤더를 확인하고 파일 끝에 추가하며 기존 레코드 전체를 재검사하지 않습니다. 저장 실패나 손상이 의심되면 추가 INSERT 전에 SELECT로 검사하고, 원본을 백업해 복구해야 합니다.

## 실행 의미와 현재 한계

- 파일 전체를 먼저 파싱합니다. 어느 문장이든 구문 오류가 있으면 아무 문장도 실행하지 않습니다.
- 실행 중 의미 오류·I/O 오류가 생기면 그 지점에서 중단합니다. 앞서 성공한 문장은 유지됩니다. 여러 문장을 묶는 트랜잭션은 없습니다.
- 쓰기는 `flush`/`close`까지 확인합니다. 전원 장애에 대한 fsync 보장, WAL, 롤백 및 자동 복구는 없습니다.
- 단일 프로세스·단일 스레드용입니다. 같은 데이터 폴더를 여러 프로세스에서 동시에 사용하지 않습니다. Phase 3의 잠금과 동시성 제어는 구현하지 않았습니다.
- SELECT 결과, SQL 스크립트와 INSERT 배치는 메모리에 보관합니다. 스트리밍 결과나 대용량 입력 분할은 후속 확장 대상입니다.
- CREATE/UPDATE/DELETE, NULL, 기본값, 자동 ID, UNIQUE, OR, 괄호 조건, JOIN, 정렬, 집계, 인덱스, API 서버는 지원하지 않습니다.

## 검증

`build.ps1 -Test` 또는 CTest는 임시 디렉터리에 데이터를 생성하므로 사용 중인 `data`를 변경하지 않습니다. C++ 테스트는 파싱·스키마 검사·실행·손상 탐지·10,000행 조회를 검증하며, CLI 테스트는 파일 입력·UTF-8·표준 입력·프로세스 재시작 후 조회·종료 코드·오류 시 저장 범위를 검증합니다. Phase 2의 100만 행 벤치마크는 포함하지 않았습니다.
