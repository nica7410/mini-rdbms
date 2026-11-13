// SQL 문자열을 구조체(AST)로 바꾸는 두 단계가 모두 이 파일에 있다.
//   1) Lexer : 글자 → 토큰 (peek로 보고, take로 소비)
//   2) Parser: 토큰 → Insert / Select 구조체 (accept로 맞으면 소비, expect로 아니면 오류)
// Parser는 토큰이 필요할 때마다 lexer_.next()를 한 번씩 불러 받는다 (토큰을 미리 다 만들지 않음).
#include "rdbms.hpp"

#include <charconv>     // from_chars: 문자열 → 숫자 (빠르고 예외 없음)
#include <stdexcept>
#include <string_view>  // 문자열을 복사 없이 "바라보는" 타입
#include <utility>

namespace mini {
	using namespace std;
	// 이름 없는 namespace: 여기부터 아래 "} // namespace" 까지는 이 파일 안에서만 쓰는 도구들
	namespace {
		// 토큰 종류 5가지: 단어(키워드·이름), 숫자, 문자열, 기호, 입력 끝
		enum class Kind { Word, Number, String, Symbol, End };
		// 토큰 = 종류 + 글자 + 위치(줄, 열). 위치는 "SQL 2:8: ..." 같은 오류 메시지용
		struct Token { 
			Kind kind; 
			string text; 
			size_t line, column; 
		};
		// 글자 분류 함수들 (로케일 영향 없이 ASCII 기준으로 직접 비교)
		bool alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
		bool digit(char c) { return c >= '0' && c <= '9'; }
		char lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c; }  // 대문자만 소문자로
		bool whitespace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v'; }

		// ===================== Lexer =====================
		// SQL 글자들을 왼쪽부터 읽어 토큰을 하나씩 만들어 준다
		class Lexer {
		public:
			explicit Lexer(const string& input) : input_(input) {
				// 파일 맨 앞의 UTF-8 BOM(EF BB BF) 3바이트는 글자가 아니므로 건너뛴다
				if (input_.compare(0, 3, "\xEF\xBB\xBF") == 0) pos_ = 3;
			}
			// 호출할 때마다 "다음 토큰 하나"를 돌려준다
			Token next() {
				skip();  // 공백과 주석을 먼저 건너뛰고
				Token token{ Kind::End, "", line_, column_ };  // 토큰 시작 위치를 기록
				if (pos_ == input_.size()) return token;  // 입력 끝이면 End 토큰
				char c = peek();  // 첫 글자를 보고 토큰 종류를 정한다
				if (alpha(c)) {
					// [Word] 문자로 시작 → 문자/숫자가 이어지는 동안 소문자로 모은다
					// (SELECT도 users도 모두 Word. 키워드인지는 Parser가 판단)
					token.kind = Kind::Word;
					while (alpha(peek()) || digit(peek())) token.text += lower(take());
				}
				else if (digit(c) || ((c == '-' || c == '+') && digit(peek(1)))) {
					// [Number] 숫자, 또는 "바로 뒤에 숫자가 오는" +/- 로 시작 (-5는 OK, - 5는 오류)
					token.kind = Kind::Number;
					token.text += take();
					while (digit(peek())) token.text += take();
				}
				else if (c == '\'') {
					// [String] 작은따옴표로 시작
					token.kind = Kind::String;
					take();  // 여는 ' 는 버린다
					while (true) {
						if (pos_ == input_.size()) fail(token, "unterminated string");  // 닫는 ' 없이 입력이 끝남
						c = take();
						if (c != '\'') {	// 일반 글자는 그대로 저장 (; -- /* 도 그냥 글자)
							token.text += c;
							continue; 
						}
						// ' 를 만났다: 바로 뒤도 ' 이면 '' → 글자 ' 하나, 아니면 문자열 끝
						if (peek() == '\'') { 
							take(); 
							token.text += '\''; 
						}
						else break;
					}
				}
				else {
					// [Symbol] 나머지는 기호. 허용 목록 9개에 없으면 오류
					token.kind = Kind::Symbol;
					token.text += take();
					if (string_view("(),;*=<>!").find(c) == string_view::npos) 
						fail(token, "unexpected character");
					// 두 글자 연산자 묶기: <= >= != <>  (Parser는 정확히 일치하는지만 보면 된다)
					if ((c == '<' || c == '>' || c == '!') && peek() == '=') token.text += take();
					else if (c == '<' && peek() == '>') token.text += take();
				}
				return token;
			}
			// 오류를 "SQL 줄:열: 메시지" 형식의 예외로 던진다.
			// [[noreturn]] = 이 함수는 정상적으로 돌아오지 않는다. static이라 Parser도 Lexer::fail로 쓴다
			[[noreturn]] static void fail(const Token& t, const string& message) {
				throw runtime_error("SQL " + to_string(t.line) + ":" + to_string(t.column) + ": " + message);
			}
		private:
			// peek: 지금(또는 offset만큼 뒤) 글자를 "보기만" 한다. 끝을 넘으면 '\0'을 돌려줘 범위 밖 접근을 막는다
			char peek(size_t offset = 0) const { return pos_ + offset < input_.size() ? input_[pos_ + offset] : '\0'; }
			// take: 글자 하나를 "소비"하고 위치(줄/열)를 갱신한다
			char take() {
				const char c = input_[pos_++];
				if (c == '\n') { 
					++line_; 
					column_ = 1; 
				}
				else ++column_;
				return c;
			}
			// 공백, -- 한 줄 주석, /* 여러 줄 주석 */ 을 건너뛰고 다음 토큰의 첫 글자에서 멈춘다
			void skip() {
				while (pos_ < input_.size()) {
					if (whitespace(peek())) { take(); continue; }
					if (peek() == '-' && peek(1) == '-') {
						// -- 이면 줄 끝까지 버린다
						while (pos_ < input_.size() && peek() != '\n') take();
						continue;
					}
					if (peek() == '/' && peek(1) == '*') {
						const Token start{ Kind::Symbol, "", line_, column_ };  // 닫히지 않았을 때 오류 위치로 쓰려고 시작점 기억
						take(); 
						take();
						while (!(peek() == '*' && peek(1) == '/')) {
							if (pos_ == input_.size()) fail(start, "unterminated comment");
							take();
						}
						take(); 
						take();
						continue;  // 닫는 */ 소비
					}
					break;  // 공백도 주석도 아니면 멈춤 → 여기서 토큰이 시작된다
				}
			}
			const string& input_;  // 원본 SQL (복사하지 않고 참조)
			size_t pos_ = 0, line_ = 1, column_ = 1;  // 현재 읽는 위치
		};

		// ===================== Parser =====================
		// 재귀 하강 파서: 문법 규칙 하나 = 함수 하나 (script, insert, select, value ...)
		class Parser {
		public:
			// 만들자마자 첫 토큰을 읽어 token_ 에 둔다. 항상 "현재 토큰" 하나를 들고 있다 (lookahead 1)
			explicit Parser(const string& input) : lexer_(input), token_(lexer_.next()) {}
			// 스크립트 = 문장 ; 문장 ; ... 의 반복 (마지막 ; 는 생략 가능)
			vector<Statement> script() {
				vector<Statement> statements;
				while (token_.kind != Kind::End) {
					if (accept(";")) continue;  // 빈 문장 ;; 은 건너뜀
					if (accept("insert")) statements.emplace_back(insert());
					else if (accept("select")) statements.emplace_back(select());
					else Lexer::fail(token_, "expected INSERT or SELECT");
					if (token_.kind != Kind::End) expect(";");  // 문장 뒤엔 ; 또는 입력 끝만 올 수 있다
				}
				return statements;
			}
		private:
			// accept: 현재 토큰이 text 이면 소비하고 true, 아니면 아무것도 안 하고 false ("있어도 되고 없어도 되는 것")
			// Word와 Symbol만 비교하므로 문자열 'select'는 키워드로 오인되지 않는다
			bool accept(const string& text) {
				if ((token_.kind != Kind::Word && token_.kind != Kind::Symbol) || token_.text != text) return false;
				token_ = lexer_.next();
				return true;
			}
			// expect: 반드시 text 여야 한다. 아니면 "expected 'text'" 오류
			void expect(const string& text) {
				if (!accept(text)) Lexer::fail(token_, "expected '" + text + "'");
			}
			// identifier: Word 토큰 하나를 이름으로 꺼낸다 (테이블명, 컬럼명)
			string identifier() {
				if (token_.kind != Kind::Word) Lexer::fail(token_, "expected identifier");
				auto name = token_.text; 
				token_ = lexer_.next(); 
				return name;
			}
			// value: 리터럴 하나 → Value (문자열이면 그대로, 숫자면 int64로)
			Value value() {
				if (token_.kind == Kind::String) {
					auto text = token_.text; 
					token_ = lexer_.next(); 
					return text;
				}
				if (token_.kind != Kind::Number) Lexer::fail(token_, "expected integer or single-quoted string");
				auto text = string_view(token_.text);
				if (text.front() == '+') text.remove_prefix(1);  // from_chars는 + 기호를 읽지 못해서 떼어 낸다
				int64_t number = 0;
				// 문자열 → int64. 범위를 넘거나(오버플로) 끝까지 소비하지 못하면 실패
				const auto result = from_chars(text.data(), text.data() + text.size(), number);
				if (result.ec != errc{} || result.ptr != text.data() + text.size())	
					Lexer::fail(token_, "integer outside signed 64-bit range");
				token_ = lexer_.next();
				return number;
			}
			// INSERT INTO 테이블 [(컬럼, ...)] VALUES (값, ...) [, (값, ...)]
			// ("insert" 키워드는 script()에서 이미 소비됨)
			Insert insert() {
				expect("into");
				Insert result{ identifier(), {}, {} };  // 테이블 이름
				if (accept("(")) {
					// 컬럼 목록(선택). do-while + accept(",") = "쉼표로 구분된 목록"을 읽는 공식
					do { result.columns.push_back(identifier()); } while (accept(","));
					expect(")");
				}
				expect("values");
				// 바깥 반복: (…), (…) 행들 / 안쪽 반복: 한 행 안의 값들
				do {
					expect("(");
					Row row;
					do { row.push_back(value()); } while (accept(","));
					expect(")"); result.rows.push_back(std::move(row));
				} while (accept(","));
				return result;
			}
			// SELECT (* | 컬럼, ...) FROM 테이블 [WHERE 조건 [AND 조건 ...]]
			Select select() {
				Select result;
				if (!accept("*")) {
					// * 가 아니면 컬럼 목록. * 이면 columns를 비워 둔다 (실행기가 "전체 컬럼"으로 해석)
					do { result.columns.push_back(identifier()); } while (accept(","));
				}
				expect("from"); result.table = identifier();
				if (accept("where")) {
					// 조건 = 컬럼 연산자 값
					do {
						const auto column = identifier();
						Compare op;
						// Lexer가 <=, >= 를 이미 한 토큰으로 묶어 두었으므로 정확히 일치하는지만 보면 된다
						if (accept("=")) op = Compare::Equal;
						else if (accept("!=") || accept("<>")) op = Compare::NotEqual;
						else if (accept("<=")) op = Compare::LessEqual;
						else if (accept(">=")) op = Compare::GreaterEqual;
						else if (accept("<")) op = Compare::Less;
						else if (accept(">")) op = Compare::Greater;
						else Lexer::fail(token_, "expected comparison operator");
						result.conditions.push_back({ column, op, value() });
					} while (accept("and"));  // AND로만 이어 붙인다. OR가 오면 여기서 끝나고 script()의 expect(";")에서 오류
				}
				return result;
			}
			Lexer lexer_;  // Parser가 Lexer를 품고 있다
			Token token_;  // 현재 토큰 (한 개만 미리 봄 = lookahead 1)
		};
	} // namespace

	// 바깥에 공개되는 유일한 함수: 임시 Parser를 만들어 script()를 부르고 결과만 돌려준다
	vector<Statement> parse(const string& sql) { return Parser(sql).script(); }
} // namespace mini