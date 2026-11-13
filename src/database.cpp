// 엔진의 본체. 아래에서 위로 세 층이 들어 있다.
//   1) 바이트 도구 : 길이 4바이트(addU32/readU32), 프레임(writeFrame/readFrame), 행 인코딩(encode/decode)
//   2) FileStore   : 테이블 파일에 추가(append) / 처음부터 끝까지 읽기(scan)
//   3) Engine      : INSERT/SELECT의 의미 처리(컬럼 매핑, 타입 검사, WHERE, 투영) + 결과 출력
//
// 테이블 파일 data/<table>.mdb 형식:
//   "MRDB001\n"(8바이트) + [스키마 서명 프레임] + [행 프레임]*
//   프레임 = [payload 길이 u32 little-endian 4바이트][payload]
//   행 payload = 컬럼마다 [값 길이 4바이트][값 바이트]  (INT는 10진수 문자열, TEXT는 원본 UTF-8)
#include "rdbms.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <fstream>   // 파일 입출력 (ifstream / ofstream)
#include <numeric>   // iota (0, 1, 2 … 채우기)
#include <set>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace mini {
	using namespace std;
	// 이름 없는 namespace: 아래 "} // namespace" 까지는 이 파일 안에서만 쓰는 도구들
	namespace {
		constexpr uint32_t maxFrame = 16 * 1024 * 1024;  // 프레임 하나의 최대 크기 16 MiB (손상된 길이로 거대한 메모리를 잡는 사고 방지)
		const string magic = "MRDB001\n";  // 모든 .mdb 파일의 첫 8바이트

		// 모든 오류는 이 함수로 예외를 던진다. main()의 catch가 받아 "error: ..."로 출력
		[[noreturn]] void fail(const string& message) { throw runtime_error(message); }

		// ===================== 이름·스키마 검사 =====================
		// 이름 규칙: [a-z_][a-z0-9_]* (소문자만).
		// 테이블 이름은 파일 이름(users.mdb)이 되므로 "../x" 같은 경로 조작을 여기서 막는다
		bool validName(const string& name) {
			auto first = [](char c) { return (c >= 'a' && c <= 'z') || c == '_'; };  // 람다: 첫 글자 규칙
			if (name.empty() || !first(name.front())) return false;
			return all_of(name.begin(), name.end(), [&](char c) { return first(c) || (c >= '0' && c <= '9'); });
		}
		// 스키마 검사: 테이블명 규칙, 컬럼 1~256개, 컬럼명 중복 없음, 타입 유효
		void validateSchema(const Schema& schema) {
			if (!validName(schema.table)) fail("Invalid table name: " + schema.table);
			if (schema.columns.empty() || schema.columns.size() > 256) fail("Schema must have 1..256 columns");
			set<string> names;
			for (const auto& c : schema.columns) {
				// set.insert(...).second 는 "새로 들어갔으면 true" → false면 이미 있던 이름(중복)
				if (!validName(c.name) || !names.insert(c.name).second) fail("Invalid or duplicate schema column: " + c.name);
				if (c.type != Type::Integer && c.type != Type::Text) fail("Invalid column type");
			}
		}
		// 스키마 서명: 스키마를 "users\nid INT\nname TEXT\nage INT\n" 같은 문자열로 정규화한다.
		// 파일 첫 프레임에 저장해 두고, 파일을 열 때마다 현재 .schema와 같은지 비교한다
		// (컬럼 순서/타입을 바꾼 뒤 기존 데이터를 엉뚱하게 해석하는 사고 방지)
		string signature(const Schema& schema) {
			validateSchema(schema);
			string out = schema.table + "\n";
			for (const auto& c : schema.columns) out += c.name + (c.type == Type::Integer ? " INT\n" : " TEXT\n");
			if (out.size() > maxFrame) fail("Schema too large");
			return out;
		}

		// ===================== 실행기 도우미 =====================
		// 값의 실제 타입(variant 안에 든 것)이 컬럼 타입과 같은지. INT 컬럼에 문자열 등은 오류
		void validateValue(const Column& column, const Value& value) {
			if ((column.type == Type::Integer) != holds_alternative<int64_t>(value))
				fail("Type mismatch for column '" + column.name + "'");
		}
		// 컬럼 이름 → 스키마에서의 위치(0부터). 예) "age" → 2. 없으면 오류
		size_t columnIndex(const Schema& schema, const string& name) {
			for (size_t i = 0; i < schema.columns.size(); ++i) if (schema.columns[i].name == name) return i;
			fail("Unknown column '" + name + "' in table '" + schema.table + "'");
		}
		// 값 → 문자열. 정수 25 → "25", 문자열은 그대로. 저장(encode)과 출력(CSV) 양쪽에서 쓴다
		string valueText(const Value& value) {
			if (const auto* number = get_if<int64_t>(&value)) return to_string(*number);  // get_if: 정수가 들었으면 포인터, 아니면 nullptr
			return get<string>(value);
		}

		// ===================== 바이트 도구 =====================
		// 32비트 정수 → 4바이트 (little-endian: 작은 자리 바이트가 먼저)
		// 예) 24 = 0x00000018 → 18 00 00 00
		// 비트 연산으로 순서를 명시했기 때문에 어떤 CPU에서 만든 파일도 같은 방식으로 읽힌다
		void addU32(string& out, uint32_t value) {
			for (int shift = 0; shift < 32; shift += 8) out += static_cast<char>((value >> shift) & 255);  // 밀고, 아래 8비트만 남김
		}
		// 4바이트 → 32비트 정수 (addU32의 반대)
		uint32_t readU32(const char* bytes) {
			uint32_t value = 0;
			// unsigned char로 바꾸는 이유: 0x80 이상인 char가 음수로 해석되어 비트가 망가지는 것을 막으려고
			for (unsigned i = 0; i < 4; ++i) value |= static_cast<uint32_t>(static_cast<unsigned char>(bytes[i])) << (8 * i);
			return value;
		}
		// 프레임 쓰기: [길이 4바이트][내용]
		// 길이를 먼저 적으므로 내용에 쉼표·줄바꿈·따옴표가 있어도 이스케이프할 필요가 없다
		void writeFrame(ostream& out, const string& payload) {
			string size;
			addU32(size, static_cast<uint32_t>(payload.size()));
			out.write(size.data(), 4);
			out.write(payload.data(), static_cast<streamsize>(payload.size()));
		}
		// 프레임 읽기: 한 프레임 읽으면 true, 파일이 정확히 끝났으면 false, 망가졌으면 예외
		bool readFrame(istream& in, string& payload) {
			array<char, 4> bytes{};
			in.read(bytes.data(), 4);
			if (in.gcount() == 0 && in.eof() && !in.bad()) return false;  // 0바이트 읽고 EOF = 정상 종료
			if (!in) fail("Corrupt table: incomplete frame length or read failure");  // 길이 4바이트 중 일부만 있음
			const auto size = readU32(bytes.data());
			if (size > maxFrame) fail("Corrupt table: frame exceeds 16 MiB");
			payload.resize(size);
			if (size != 0) in.read(payload.data(), static_cast<streamsize>(size));
			if (!in) fail("Corrupt table: truncated frame or read failure");  // 적힌 길이보다 내용이 짧음
			return true;
		}
		// 파일 앞부분 검사: 매직 8바이트 + 첫 프레임(스키마 서명)이 기대값과 같은지
		void checkHeader(istream& in, const string& expected) {
			string bytes(magic.size(), '\0');
			in.read(bytes.data(), static_cast<streamsize>(bytes.size()));
			if (!in || bytes != magic) fail("Corrupt table: invalid format header");
			if (!readFrame(in, bytes) || bytes != expected) fail("Table schema does not match the stored schema");
		}
		// Row → payload 바이트. 필드마다 [길이 4바이트][값 바이트]를 이어 붙인다
		string encode(const Schema& schema, const Row& row) {
			if (row.size() != schema.columns.size()) fail("Row does not match schema column count");
			string payload;
			for (size_t i = 0; i < row.size(); ++i) {
				validateValue(schema.columns[i], row[i]);
				const auto text = valueText(row[i]);  // 정수도 "25"처럼 10진수 문자열로 저장한다
				if (text.size() > maxFrame || payload.size() + 4 + text.size() > maxFrame) fail("Row exceeds 16 MiB");
				addU32(payload, static_cast<uint32_t>(text.size()));
				payload += text;
			}
			return payload;
		}
		// payload 바이트 → Row (encode의 반대). 스키마 컬럼 순서대로 "길이 → 그만큼의 값"을 잘라 낸다
		Row decode(const Schema& schema, const string& payload) {
			Row row;
			size_t pos = 0;  // payload 안에서 지금 읽는 위치
			for (const auto& c : schema.columns) {
				if (payload.size() - pos < 4) fail("Corrupt table: missing field length");
				const auto size = readU32(payload.data() + pos); pos += 4;
				if (size > payload.size() - pos) fail("Corrupt table: truncated field");
				const auto text = payload.substr(pos, size); pos += size;
				if (c.type == Type::Text) row.emplace_back(text);
				else {
					// INT 컬럼: "25" → 25. 숫자가 아니거나 범위를 넘으면 손상으로 본다
					int64_t number = 0;
					const auto result = from_chars(text.data(), text.data() + text.size(), number);
					if (result.ec != errc{} || result.ptr != text.data() + text.size()) fail("Corrupt table: invalid integer");
					row.emplace_back(number);
				}
			}
			if (pos != payload.size()) fail("Corrupt table: extra bytes in row");  // 다 읽었는데 바이트가 남아도 손상
			return row;
		}
		// 두 Value 비교 (WHERE 조건 하나 평가). variant끼리의 ==, < 를 그대로 쓴다.
		// 실행 전에 타입이 같음을 검증했으므로 정수는 수치로(10 > 9), 문자열은 바이트 순서로 비교된다
		bool compare(const Value& a, Compare op, const Value& b) {
			switch (op) {
			case Compare::Equal: return a == b;
			case Compare::NotEqual: return a != b;
			case Compare::Less: return a < b;
			case Compare::LessEqual: return a <= b;
			case Compare::Greater: return a > b;
			case Compare::GreaterEqual: return a >= b;
			}
			fail("Invalid comparison operator");
		}
		// CSV 한 칸 만들기: 쉼표, 큰따옴표, 줄바꿈이 있을 때만 "..."로 감싸고 안쪽 " 는 "" 로 바꾼다
		string csv(const string& text) {
			if (text.find_first_of(",\"\r\n") == string::npos) return text;  // 특수 문자가 없으면 그대로
			string out = "\"";
			for (char c : text) { if (c == '\"') out += '\"'; out += c; }
			return out + "\"";
		}
	} // namespace

	// ===================== 스키마 로딩 =====================
	// schemas/<table>.schema 를 한 줄씩 읽어 Schema로 만든다. CREATE TABLE 대신 이 파일이 테이블을 정의한다
	// 한 줄 형식: "컬럼명 타입"   (# 뒤는 주석, 빈 줄은 무시)
	Schema loadSchema(const filesystem::path& directory, const string& table) {
		if (!validName(table)) fail("Invalid table name: " + table);
		const auto path = directory / (table + ".schema");  // path의 / 연산자 = 경로 이어 붙이기
		ifstream in(path, ios::binary);
		if (!in) fail("Cannot open schema: " + path.string());  // 스키마 파일이 없음 = 테이블이 없음
		Schema schema{ table, {} };
		string line;
		size_t lineNumber = 0;
		while (getline(in, line)) {
			++lineNumber;
			if (lineNumber == 1 && line.compare(0, 3, "\xEF\xBB\xBF") == 0) line.erase(0, 3);  // UTF-8 BOM 제거
			const auto comment = line.find('#');
			if (comment != string::npos) line.erase(comment);  // # 뒤는 주석이라 잘라 낸다
			istringstream fields(line);  // 공백 기준으로 단어를 하나씩 꺼내는 스트림
			string name, type, extra;
			if (!(fields >> name)) continue;  // 빈 줄은 건너뜀
			// 타입이 없거나, 세 번째 단어가 있거나, INT/TEXT가 아니면 "파일:줄"과 함께 오류
			if (!(fields >> type) || (fields >> extra) || (type != "INT" && type != "TEXT"))
				fail("Invalid schema at " + path.string() + ":" + to_string(lineNumber));
			schema.columns.push_back({ name, type == "INT" ? Type::Integer : Type::Text });
		}
		if (in.bad()) fail("Failed to read schema: " + path.string());
		validateSchema(schema);  // 컬럼 개수, 이름 규칙, 중복 검사
		return schema;
	}

	// ===================== FileStore =====================
	FileStore::FileStore(filesystem::path directory) : directory_(std::move(directory)) {}

	// 행들을 테이블 파일 끝에 추가한다 (INSERT의 실제 저장 담당)
	void FileStore::append(const Schema& schema, const vector<Row>& rows) {
		const auto expected = signature(schema);
		// Validate the whole batch before opening the file for writing.
		// ① 모든 행을 메모리에서 먼저 인코딩한다. 여기서 실패하면 파일은 전혀 건드리지 않는다
		//    (3행 중 마지막 행이 틀려도 앞의 2행만 저장되는 일이 없다)
		vector<string> encoded;
		encoded.reserve(rows.size());
		for (const auto& row : rows) encoded.push_back(encode(schema, row));
		if (encoded.empty()) return;
		filesystem::create_directories(directory_);  // data/ 폴더가 없으면 만든다
		const auto path = directory_ / (schema.table + ".mdb");
		const bool exists = filesystem::exists(path);
		if (exists) {
			// ② 이미 있는 파일이면 헤더(매직 + 스키마 서명)가 맞는지 확인
			ifstream in(path, ios::binary);
			if (!in) fail("Cannot read table: " + path.string());
			checkHeader(in, expected);
		}
		// ③ app 모드 = 항상 파일 끝에 이어 쓰기 (기존 레코드는 다시 쓰지 않는다)
		ofstream out(path, ios::binary | ios::app);
		if (!out) fail("Cannot write table: " + path.string());
		// 새 파일이면 매직 헤더와 스키마 서명 프레임부터 쓴다
		if (!exists) { out.write(magic.data(), static_cast<streamsize>(magic.size())); writeFrame(out, expected); }
		for (const auto& row : encoded) writeFrame(out, row);  // ④ 행 프레임들
		// ⑤ 디스크 꽉 참 같은 쓰기 실패를 놓치지 않도록 flush/close 후 상태를 확인
		out.flush();
		if (!out) fail("Table write failed; partial data may exist: " + path.string());
		out.close();
		if (!out) fail("Table close failed: " + path.string());
	}

	// 테이블 파일을 처음부터 끝까지 읽으며 행마다 visit(행)을 호출한다 (SELECT의 순차 탐색)
	// 어떤 행을 고를지는 visit을 넘긴 쪽(Engine)이 결정한다
	void FileStore::scan(const Schema& schema, const function<void(const Row&)>& visit) const {
		const auto expected = signature(schema);
		const auto path = directory_ / (schema.table + ".mdb");
		if (!filesystem::exists(path)) return; // Existing schema with no records yet.
		ifstream in(path, ios::binary);
		if (!in) fail("Cannot read table: " + path.string());
		checkHeader(in, expected);
		string payload;
		while (readFrame(in, payload)) visit(decode(schema, payload));  // 한 행씩: 읽기 → 디코딩 → 콜백 (전체를 메모리에 올리지 않음)
	}

	// ===================== Engine =====================
	Engine::Engine(filesystem::path schemas, filesystem::path data)
		: schemas_(std::move(schemas)), store_(std::move(data)) {}

	// 문장 하나를 실행한다
	Result Engine::execute(const Statement& statement) {
		// ---------- INSERT ----------
		// get_if: statement 안에 Insert가 들었으면 포인터, 아니면 nullptr
		if (const auto* insert = get_if<Insert>(&statement)) {
			const auto schema = loadSchema(schemas_, insert->table);
			// mapping[i] = "사용자가 쓴 i번째 값이 들어갈 스키마 위치"
			// 예) INSERT INTO users (age, name, id) → mapping = [2, 1, 0]
			vector<size_t> mapping;
			if (insert->columns.empty()) {
				// 컬럼 목록 생략 → [0, 1, 2, ...] (스키마 순서 그대로)
				mapping.resize(schema.columns.size()); iota(mapping.begin(), mapping.end(), 0);
			}
			else {
				set<string> seen;
				for (const auto& name : insert->columns) {
					if (!seen.insert(name).second) fail("Duplicate INSERT column: " + name);
					mapping.push_back(columnIndex(schema, name));
				}
				// 모든 컬럼을 정확히 한 번씩 줘야 한다 (기본값/NULL 미지원)
				if (mapping.size() != schema.columns.size()) fail("INSERT must supply every column; defaults and NULL are unsupported");
			}
			if (insert->rows.empty()) fail("INSERT requires at least one row");
			vector<Row> rows;
			for (const auto& values : insert->rows) {
				if (values.size() != mapping.size()) fail("INSERT value count does not match column count");
				Row row(schema.columns.size());  // 스키마 크기의 빈 행
				for (size_t i = 0; i < values.size(); ++i) {
					// 타입을 확인한 뒤, 스키마 위치에 값을 꽂는다 → 저장은 항상 스키마 순서
					validateValue(schema.columns[mapping[i]], values[i]); row[mapping[i]] = values[i];
				}
				rows.push_back(std::move(row));
			}
			store_.append(schema, rows);  // 전부 검증된 행들만 저장소로
			Result result; result.inserted = rows.size(); return result;
		}
		// ---------- SELECT ----------
		// 여기 왔으면 Select다. get<Select>로 꺼낸다
		const auto& select = get<Select>(statement);
		const auto schema = loadSchema(schemas_, select.table);
		Result result; result.isSelect = true;
		// 컬럼 이름을 미리 "인덱스"로 바꿔 둔다 → 스캔 중 행마다 이름을 찾지 않아도 된다
		//   projection = 출력할 컬럼 위치들, conditions = WHERE 조건 컬럼 위치들
		vector<size_t> projection, conditions;
		if (select.columns.empty()) {
			// SELECT * → 모든 컬럼
			for (size_t i = 0; i < schema.columns.size(); ++i) {
				projection.push_back(i); result.columns.push_back(schema.columns[i].name);
			}
		}
		else {
			for (const auto& name : select.columns) { projection.push_back(columnIndex(schema, name)); result.columns.push_back(name); }
		}
		for (const auto& condition : select.conditions) {
			const auto index = columnIndex(schema, condition.column);
			// WHERE age = 'abc' 같은 타입 오류는 데이터를 읽기 전에 막는다
			validateValue(schema.columns[index], condition.value); conditions.push_back(index);
		}
		// 람다 [&]: 바깥 변수(result, conditions, projection …)를 참조로 쓰는 콜백. scan이 행마다 호출한다
		store_.scan(schema, [&](const Row& row) {
			// AND 단락 평가: 조건 하나라도 거짓이면 return → 이 행은 버리고 다음 행으로
			for (size_t i = 0; i < conditions.size(); ++i)
				if (!compare(row[conditions[i]], select.conditions[i].op, select.conditions[i].value)) return;
			Row projected;
			for (auto index : projection) projected.push_back(row[index]);  // 필요한 컬럼만 골라 담기
			result.rows.push_back(std::move(projected));
			});
		return result;
	}

	// ===================== 출력 =====================
	// Result → 출력 문자열. INSERT면 "INSERT 3", SELECT면 헤더가 있는 CSV (결과가 0행이어도 헤더는 출력)
	string formatResult(const Result& result) {
		if (!result.isSelect) return "INSERT " + to_string(result.inserted) + "\n";
		ostringstream out;
		// 헤더 줄. (i ? "," : "") = 첫 칸 앞에는 쉼표를 붙이지 않는 요령
		for (size_t i = 0; i < result.columns.size(); ++i) out << (i ? "," : "") << csv(result.columns[i]);
		out << '\n';
		// 데이터 줄들
		for (const auto& row : result.rows) {
			for (size_t i = 0; i < row.size(); ++i) out << (i ? "," : "") << csv(valueText(row[i]));
			out << '\n';
		}
		return out.str();
	}
} // namespace mini