#pragma once
// 이 헤더를 여러 파일이 #include 해도 컴파일러는 한 번만 읽는다.
// 프로젝트 전체의 "설계도": 데이터 구조와 바깥에 공개되는 함수/클래스를 선언만 한다.

#include <cstdint>     // int64_t: 크기가 정확히 64비트인 정수
#include <filesystem>  // filesystem::path: OS에 맞게 경로를 다룬다
#include <functional>  // function: "함수를 담는 변수" 타입 (scan 콜백용)
#include <string>
#include <variant>     // variant: "A 아니면 B"를 담는 타입
#include <vector>

// 이 프로젝트의 모든 이름은 mini:: 아래에 둔다 (다른 라이브러리와 이름 충돌 방지)
namespace mini {
	// mini 안에서는 std:: 를 생략하고 string, vector 처럼 쓴다.
	// mini 이름공간 안에만 적용되므로, 이 헤더를 include 한 쪽의 전역 공간에는 퍼지지 않는다.
	using namespace std;

	// ===== 데이터 모양 =====
	// [값] 한 칸에 들어가는 값: 정수(int64) 또는 문자열 둘 중 하나
	using Value = variant<int64_t, string>;
	// [행] 값들의 배열. 예) [1, "김정글", 25]. 어느 칸이 어느 컬럼인지는 순서로 Schema와 짝을 맞춘다
	using Row = vector<Value>;
	// 컬럼 타입은 딱 두 가지: INT(Integer)와 TEXT(Text)
	enum class Type { Integer, Text };
	// 컬럼 = 이름 + 타입. 예) { "age", Integer }
	struct Column { string name; Type type; };
	// 스키마 = 테이블 이름 + 컬럼 목록 (schemas/<table>.schema 파일을 읽은 결과)
	struct Schema { string table; vector<Column> columns; };

	// ===== SQL 문장을 파싱한 결과 (AST) =====
	// WHERE에서 쓸 수 있는 비교 연산자 6종
	enum class Compare { Equal, NotEqual, Less, LessEqual, Greater, GreaterEqual };
	// 조건 하나: "age >= 25" → { "age", GreaterEqual, 25 }
	struct Condition { string column; Compare op; Value value; };
	// INSERT 문을 파싱한 결과
	struct Insert {
		string table;
		vector<string> columns; // Empty means schema order.
		vector<Row> rows;  // VALUES (...), (...) 의 괄호 하나가 Row 하나
	};
	// SELECT 문을 파싱한 결과
	struct Select {
		string table;
		vector<string> columns; // Empty means SELECT *.
		vector<Condition> conditions; // AND conjunction.
	};
	// 문장 하나 = Insert 아니면 Select (둘 중 하나만 담김)
	using Statement = variant<Insert, Select>;

	// Parse the entire script before executing any statement.
	// SQL 문자열 전체 → 문장 목록. 구현은 sql.cpp
	vector<Statement> parse(const string& sql);
	// schemas/<table>.schema 파일을 읽어 Schema로 만든다. 구현은 database.cpp
	Schema loadSchema(const filesystem::path& directory, const string& table);

	// Binary table storage. No SQL or output formatting lives in this layer.
	// [저장소 계층] 파일 읽기/쓰기만 담당한다. SQL, WHERE, 출력 형식은 모른다.
	class FileStore {
	public:
		explicit FileStore(filesystem::path directory);  // data/ 폴더 경로를 기억
		void append(const Schema& schema, const vector<Row>& rows);  // 행들을 테이블 파일 끝에 추가
		void scan(const Schema& schema, const function<void(const Row&)>& visit) const;  // 행을 하나씩 읽어 visit(행) 호출
	private:
		filesystem::path directory_;  // 멤버 변수는 이름 끝에 _ 를 붙이는 규칙
	};

	// 실행 결과. INSERT면 inserted만, SELECT면 columns/rows를 채운다
	struct Result {
		bool isSelect = false;
		size_t inserted = 0;     // INSERT로 추가한 행 수
		vector<string> columns;  // SELECT 결과의 헤더 (name, age ...)
		vector<Row> rows;        // SELECT 결과 행들
	};

	// [실행 계층] Statement를 받아 스키마를 확인하고 FileStore를 부린다
	class Engine {
	public:
		Engine(filesystem::path schemas, filesystem::path data);
		Result execute(const Statement& statement);  // 이 함수 하나가 엔진의 입구 (INSERT/SELECT 모두)
	private:
		filesystem::path schemas_;  // schemas/ 폴더
		FileStore store_;           // Engine 안에 FileStore가 들어 있다 (has-a)
	};

	// Result → 화면에 찍을 문자열 ("INSERT 3" 또는 헤더가 있는 CSV)
	string formatResult(const Result& result);
} // namespace mini