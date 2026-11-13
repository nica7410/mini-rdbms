// 프로그램 시작점(CLI).
// 명령줄 인자를 해석해 SQL 문자열을 구하고, 파싱 → 실행 → 출력 순서로 엔진을 부린다.
// SQL의 의미는 전혀 모르고 입력/출력과 종료 코드만 책임진다.
#include "rdbms.hpp"  // 엔진의 공개 인터페이스

#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>

using namespace std;

// 이름 없는 namespace: 이 안의 함수는 main.cpp 안에서만 보인다
namespace {
	// 스트림(파일 또는 표준 입력)의 내용을 EOF까지 통째로 문자열로 읽는다
	string readAll(istream& in) {
		string text{ istreambuf_iterator<char>(in), istreambuf_iterator<char>() };
		if (in.bad()) throw runtime_error("Failed to read SQL input");
		return text;
	}
	// --help 일 때 출력하는 사용법
	void usage() {
		cout << "mini-rdbms --schema-dir DIR --data-dir DIR [--file FILE | --execute SQL]\n"
			"Defaults: --schema-dir schemas --data-dir data\n"
			"Without --file/--execute, read a SQL script from standard input until EOF.\n"
			"SELECT outputs CSV; INSERT outputs INSERT <row count>.\n";
	}
} // namespace

// argc = 인자 개수, argv = 인자 문자열 배열. argv[0]은 프로그램 이름
int main(int argc, char** argv) {
	try {
		// ===== 1. 명령줄 인자 해석 =====
		// 기본값: schemas/ 와 data/ 폴더
		filesystem::path schemas = "schemas", data = "data", file;
		string sql;
		bool hasInput = false, fromFile = false;
		// argv[1]부터 하나씩 본다. 옵션은 "이름 값" 두 칸씩
		for (int i = 1; i < argc; ++i) {
			const string arg = argv[i];
			if (arg == "--help" || arg == "-h") { 
				usage(); 
				return 0; 
			}
			// 4개 옵션이 아니면 바로 오류
			if (arg != "--schema-dir" && arg != "--data-dir" && arg != "--file" && arg != "--execute")
				throw runtime_error("Unknown option: " + arg);
			// i를 한 칸 옮겨 옵션의 "값"을 가리킨다. 더 없으면 값 누락
			if (++i == argc) throw runtime_error("Missing value for " + arg);
			if (arg == "--schema-dir") schemas = argv[i];
			else if (arg == "--data-dir") data = argv[i];
			else {
				// --file 과 --execute 는 둘 중 하나만, 한 번만 쓸 수 있다
				if (hasInput) throw runtime_error("Use only one --file or --execute option");
				hasInput = true; 
				fromFile = arg == "--file";
				if (fromFile) file = argv[i]; 
				else sql = argv[i];
			}
		}
		// ===== 2. SQL 문자열 확보 =====
		// 파일이면 읽기 / --execute면 이미 sql에 있음 / 둘 다 없으면 표준 입력
		if (fromFile) {
			ifstream in(file, ios::binary);  // 바이너리 모드: 줄바꿈 변환 없이 그대로 읽는다
			if (!in) throw runtime_error("Cannot open SQL file: " + file.string());
			sql = readAll(in);
		}
		else if (!hasInput) sql = readAll(cin);
		// ===== 3. 파싱 → 실행 → 출력 =====
		// 전체를 먼저 파싱한다. 구문 오류가 하나라도 있으면 여기서 예외 → 아무 문장도 실행되지 않는다
		const auto statements = mini::parse(sql);
		// 엔진 생성 (폴더 경로만 기억하고 아직 파일을 열지 않는다)
		mini::Engine engine(schemas, data);
		// 문장마다 실행 → 문자열로 변환 → 출력.
		// 실행 중 오류가 나면 그 문장에서 멈추고, 앞서 성공한 문장의 결과는 이미 저장·출력된 상태로 남는다
		for (const auto& statement : statements) cout << mini::formatResult(engine.execute(statement));
		cout.flush();
		if (!cout) throw runtime_error("Failed to write output");
		return 0;  // 성공 = 종료 코드 0
	}
	catch (const exception& error) {
		// 위 어디서든(파서, 스키마, 저장소) 던진 예외가 여기로 모인다.
		// 오류는 stdout이 아닌 stderr로 보내 정상 결과(CSV)와 섞이지 않게 한다
		cerr << "error: " << error.what() << '\n';
		return 1;  // 실패 = 종료 코드 1
	}
}