# mini-rdbms Phase 1 요약 (팀 공유용)

> 자세한 내용은 [phase1-spec.md](phase1-spec.md) 참고

## 한 줄 소개
C++17 표준 라이브러리만으로 만든 **파일 기반 미니 SQL 처리기**입니다. SQL을 넣으면 파일에 저장하고, 조회하면 CSV로 돌려줍니다.

## 무엇을 할 수 있나요?

```sql
INSERT INTO users VALUES (1, '김정글', 25);
INSERT INTO users (name, age, id) VALUES ('Alice', 22, 3), ('Bob', 30, 4);

SELECT * FROM users;
SELECT name, age FROM users WHERE age >= 20 AND id <> 2;
```

- 지원: `INSERT`(여러 행 가능), `SELECT`(`WHERE ... AND ...`)
- 비교 연산자: `=` `!=` `<>` `<` `<=` `>` `>=`
- 값 타입: `INT`(64비트 정수), `TEXT`(문자열, 한글 OK)
- 키워드·컬럼명은 대소문자 구분 없음, `--` 와 `/* */` 주석 지원

## 사용법

```powershell
.\build.ps1 -Test                                          # 빌드 + 테스트
.\build-native\mini-rdbms.exe --file examples/demo.sql      # SQL 파일 실행
.\build-native\mini-rdbms.exe --execute "SELECT * FROM users;"   # SQL 직접 실행
```

- 입력 방법: `--file`, `--execute`, 또는 표준 입력(stdin)
- 성공: 종료 코드 `0` / 실패: `error: ...` 출력 후 종료 코드 `1`
- 폴더 지정: `--schema-dir`(기본 `schemas`), `--data-dir`(기본 `data`)

## 테이블은 어떻게 만드나요?
`CREATE TABLE`이 없습니다. `schemas/users.schema` 같은 파일을 만들면 테이블이 생깁니다.

```text
id INT
name TEXT
age INT
```

## 어떻게 동작하나요?

```text
SQL 입력 → 렉서(토큰 분리) → 파서(구조 만들기) → 엔진(검사·실행) → 파일 저장소 → 결과 출력
```

| 파일 | 역할 |
|---|---|
| `src/main.cpp` | 명령줄 처리, 입력/출력 |
| `src/sql.cpp` | SQL 읽기(렉서 + 파서) |
| `src/database.cpp` | 스키마, 파일 읽기/쓰기, 실행, CSV 출력 |
| `include/rdbms.hpp` | 공용 타입과 인터페이스 |
| `tests/` | C++ 테스트 + CLI 테스트 |

데이터는 `data/<테이블>.mdb` 바이너리 파일에 **파일 끝에 계속 추가(append)** 하는 방식으로 저장하고, 조회는 처음부터 끝까지 훑습니다.

## 꼭 알아둘 동작
- **구문 오류가 하나라도 있으면 아무 문장도 실행하지 않습니다.**
- 실행 중 오류가 나면 거기서 멈추고, **앞에서 성공한 문장은 그대로 저장**됩니다. (트랜잭션 없음)
- 한 INSERT 안에서 값이 하나라도 잘못되면 그 INSERT는 통째로 저장되지 않습니다.
- INSERT는 **모든 컬럼을 빠짐없이** 넣어야 합니다. (기본값, NULL 없음)
- 스키마를 바꾼 뒤 기존 데이터 파일을 읽으면 오류가 납니다. (마이그레이션 없음)
- 파일이 손상되면 잘림, 길이 이상, 헤더 불량 등을 감지해 오류로 알려줍니다.

## 아직 안 되는 것 (Phase 1 한계)
- `UPDATE`, `DELETE`, `CREATE`, `JOIN`, `OR`, 정렬, 집계, 인덱스
- `id` 자동 발급과 중복 방지 (지금은 직접 넣는 일반 숫자 컬럼)
- 동시 접근 보호: **한 번에 하나의 프로세스만** 사용하세요.
- 전원 장애 복구, 롤백
- 조회는 항상 전체 스캔이라 데이터가 많아지면 느립니다.

## 다음 단계 (Phase 2 예정)
- `id` 자동 발급 및 유일성 보장
- B+ 트리 인덱스로 `WHERE id = ...` 조회 빠르게 하기
- 100만 행 성능 측정
- API 서버 (엔진은 CLI와 분리되어 있어 그대로 재사용 가능)
