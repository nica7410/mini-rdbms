#include "rdbms.hpp"

#include <chrono>
#include <fstream>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>

using namespace std;

namespace {
void check(bool value, const string& message = "assertion failed") {
    if (!value) throw runtime_error(message);
}
template<class F> void rejects(F action, const string& expected) {
    try { action(); } catch (const exception& error) {
        check(string(error.what()).find(expected) != string::npos,
              "Expected error containing '" + expected + "', got: " + error.what());
        return;
    }
    throw runtime_error("Expected an exception: " + expected);
}
void write(const filesystem::path& path, const string& text) {
    ofstream out(path, ios::binary); out << text;
    if (!out) throw runtime_error("Fixture write failed");
}
struct Fixture {
    filesystem::path root;
    Fixture() {
        root = filesystem::temp_directory_path() / ("mini-rdbms-test-" +
            to_string(chrono::steady_clock::now().time_since_epoch().count()) + "-" + to_string(random_device{}()));
        if (!filesystem::create_directory(root)) throw runtime_error("Cannot create unique test directory");
        filesystem::create_directory(root / "schemas");
        write(root / "schemas/users.schema", "id INT\nname TEXT\nage INT\n");
    }
    ~Fixture() { error_code ignored; filesystem::remove_all(root, ignored); }
    mini::Engine engine() const { return mini::Engine(root / "schemas", root / "data"); }
    mini::Result run(const string& sql) const {
        auto db = engine(); return db.execute(mini::parse(sql).at(0));
    }
    filesystem::path table() const { return root / "data/users.mdb"; }
};
int passed = 0, failed = 0;
template<class F> void test(const string& name, F action) {
    try { action(); ++passed; cout << "PASS " << name << '\n'; }
    catch (const exception& e) { ++failed; cerr << "FAIL " << name << ": " << e.what() << '\n'; }
}
} // namespace

int main() {
    test("empty scripts and comments", [] { check(mini::parse("; -- hello\n /* block */ ;").empty()); });
    test("case-insensitive SQL and identifiers", [] {
        const auto s = mini::parse("SeLeCt NAME FROM USERS WhErE ID=+12");
        const auto& q = get<mini::Select>(s.at(0));
        check(q.table == "users" && q.columns.at(0) == "name");
        check(get<int64_t>(q.conditions.at(0).value) == 12);
    });
    test("UTF-8 BOM and CRLF", [] { check(mini::parse("\xEF\xBB\xBFSELECT *\r\nFROM users;").size() == 1); });
    test("strings preserve SQL punctuation and quotes", [] {
        const auto parsed = mini::parse("INSERT INTO users VALUES (1, 'a;--/* O''Brien */', 2);");
        check(get<string>(get<mini::Insert>(parsed[0]).rows[0][1]) == "a;--/* O'Brien */");
    });
    test("signed integer boundaries", [] {
        const auto parsed = mini::parse("INSERT INTO t VALUES (-9223372036854775808,9223372036854775807);");
        const auto& row = get<mini::Insert>(parsed[0]).rows[0];
        check(get<int64_t>(row[0]) == numeric_limits<int64_t>::min());
        check(get<int64_t>(row[1]) == numeric_limits<int64_t>::max());
    });
    test("integer overflow rejected", [] {
        rejects([] { mini::parse("INSERT INTO t VALUES (9223372036854775808)"); }, "64-bit");
        rejects([] { mini::parse("INSERT INTO t VALUES (-9223372036854775809)"); }, "64-bit");
    });
    test("unterminated strings and comments rejected", [] {
        rejects([] { mini::parse("INSERT INTO t VALUES ('oops)"); }, "unterminated string");
        rejects([] { mini::parse("/* oops"); }, "unterminated comment");
    });
    test("missing separator and trailing garbage rejected", [] {
        rejects([] { mini::parse("SELECT * FROM users SELECT * FROM users"); }, "expected ';'");
        rejects([] { mini::parse("SELECT * FROM users; garbage"); }, "INSERT or SELECT");
    });
    test("unsupported SQL and malformed lists rejected", [] {
        for (const auto* sql : {"CREATE TABLE t (id INT)", "SELECT FROM users", "SELECT * FROM users WHERE id=1 OR id=2",
                                "INSERT INTO users VALUES ()", "INSERT INTO users VALUES (1,)", "SELECT * FROM users WHERE id=NULL"})
            rejects([&] { mini::parse(sql); }, "SQL");
    });
    test("error includes line and column", [] {
        rejects([] { mini::parse("-- comment\nSELECT @ FROM users"); }, "SQL 2:8");
    });
    test("existing schema starts as empty table", [] {
        Fixture f; check(mini::formatResult(f.run("SELECT * FROM users")) == "id,name,age\n");
        check(!filesystem::exists(f.table()));
    });
    test("multi-row insertion and engine restart", [] {
        Fixture f; check(f.run("INSERT INTO users VALUES (1,'정글',20),(2,'O''Brien',30)").inserted == 2);
        check(mini::formatResult(f.run("SELECT * FROM users")) == "id,name,age\n1,정글,20\n2,O'Brien,30\n");
    });
    test("append preserves earlier records", [] {
        Fixture f; f.run("INSERT INTO users VALUES (1,'A',10)"); f.run("INSERT INTO users VALUES (2,'B',20)");
        check(f.run("SELECT * FROM users").rows.size() == 2);
    });
    test("explicit column order and projected output", [] {
        Fixture f; f.run("INSERT INTO users (age,name,id) VALUES (21,'A',7)");
        check(mini::formatResult(f.run("SELECT name,id FROM users")) == "name,id\nA,7\n");
    });
    test("comparison operators and AND", [] {
        Fixture f; f.run("INSERT INTO users VALUES (1,'A',10),(2,'B',20),(3,'C',30)");
        for (const auto& pair : vector<pair<string, size_t>>{
                {"=",1}, {"!=",2}, {"<>",2}, {"<",1}, {"<=",2}, {">",1}, {">=",2}})
            check(f.run("SELECT * FROM users WHERE age " + pair.first + " 20").rows.size() == pair.second);
        check(mini::formatResult(f.run("SELECT id FROM users WHERE age>=20 AND name<'C'")) == "id\n2\n");
        check(f.run("SELECT * FROM users WHERE id=99").rows.empty());
    });
    test("numeric comparisons are not lexicographic", [] {
        Fixture f; f.run("INSERT INTO users VALUES (1,'A',2),(2,'B',10)");
        check(mini::formatResult(f.run("SELECT id FROM users WHERE age>9")) == "id\n2\n");
    });
    test("text, commas, quotes, newlines, empty string round trip", [] {
        Fixture f; f.run("INSERT INTO users VALUES (1,'a,\"b\"\nc',1),(2,'',2)");
        check(mini::formatResult(f.run("SELECT name FROM users")) == "name\n\"a,\"\"b\"\"\nc\"\n\n");
    });
    test("type errors do not modify data", [] {
        Fixture f; f.run("INSERT INTO users VALUES (1,'A',1)");
        const auto size = filesystem::file_size(f.table());
        rejects([&] { f.run("INSERT INTO users VALUES (2,'B',2),(3,'C','bad')"); }, "Type mismatch");
        check(filesystem::file_size(f.table()) == size);
        rejects([&] { f.run("SELECT * FROM users WHERE id='1'"); }, "Type mismatch");
    });
    test("column errors on empty tables", [] {
        Fixture f;
        rejects([&] { f.run("SELECT missing FROM users"); }, "Unknown column");
        rejects([&] { f.run("SELECT * FROM users WHERE missing=1"); }, "Unknown column");
        rejects([&] { f.run("INSERT INTO users (id,id,age) VALUES (1,2,3)"); }, "Duplicate");
        rejects([&] { f.run("INSERT INTO users (id,name) VALUES (1,'A')"); }, "every column");
        rejects([&] { f.run("INSERT INTO users VALUES (1,'A')"); }, "value count");
        rejects([&] { f.run("INSERT INTO users (id,name,missing) VALUES (1,'A',3)"); }, "Unknown column");
        check(!filesystem::exists(f.table()));
    });
    test("unknown table and unsafe table name rejected", [] {
        Fixture f; rejects([&] { f.run("SELECT * FROM nonexistent"); }, "Cannot open schema");
        rejects([&] { mini::loadSchema(f.root / "schemas", "../users"); }, "Invalid table name");
    });
    test("schema validation", [] {
        Fixture f;
        for (const auto* schema : {"", "id INT\nid TEXT\n", "../id INT\n", "id FLOAT\n", "id INT extra\n"}) {
            write(f.root / "schemas/bad.schema", schema);
            rejects([&] { f.run("SELECT * FROM bad"); }, "");
        }
    });
    test("schema comments and BOM supported", [] {
        Fixture f; write(f.root / "schemas/bom.schema", "\xEF\xBB\xBF# header\r\nid INT # key\r\n");
        check(f.run("SELECT * FROM bom").columns.at(0) == "id");
    });
    test("schema changes cannot reinterpret stored rows", [] {
        Fixture f; f.run("INSERT INTO users VALUES (1,'A',1)");
        write(f.root / "schemas/users.schema", "id INT\nname TEXT\nheight INT\n");
        rejects([&] { f.run("SELECT * FROM users"); }, "schema does not match");
        rejects([&] { f.run("INSERT INTO users VALUES (2,'B',2)"); }, "schema does not match");
    });
    test("separate table files", [] {
        Fixture f; write(f.root / "schemas/items.schema", "id INT\n");
        f.run("INSERT INTO users VALUES (1,'A',1)"); f.run("INSERT INTO items VALUES (9)");
        check(mini::formatResult(f.run("SELECT * FROM items")) == "id\n9\n");
        check(f.run("SELECT * FROM users").rows.size() == 1);
    });
    test("truncated frame rejected", [] {
        Fixture f; f.run("INSERT INTO users VALUES (1,'A',1)");
        filesystem::resize_file(f.table(), filesystem::file_size(f.table()) - 1);
        rejects([&] { f.run("SELECT * FROM users"); }, "truncated frame");
    });
    test("partial length and oversized frame rejected", [] {
        Fixture f; f.run("INSERT INTO users VALUES (1,'A',1)");
        const auto size = filesystem::file_size(f.table());
        { ofstream out(f.table(), ios::binary | ios::app); out.put('x'); }
        rejects([&] { f.run("SELECT * FROM users"); }, "incomplete frame length");
        filesystem::resize_file(f.table(), size);
        { ofstream out(f.table(), ios::binary | ios::app); out.write("\xff\xff\xff\xff", 4); }
        rejects([&] { f.run("SELECT * FROM users"); }, "exceeds 16 MiB");
    });
    test("bad file header rejected", [] {
        Fixture f; filesystem::create_directory(f.root / "data"); write(f.table(), "not a database");
        rejects([&] { f.run("SELECT * FROM users"); }, "invalid format header");
    });
    test("oversized row rejected before file creation", [] {
        Fixture f;
        mini::FileStore store(f.root / "data");
        const auto schema = mini::loadSchema(f.root / "schemas", "users");
        rejects([&] { store.append(schema, {{int64_t{1}, string(16 * 1024 * 1024, 'x'), int64_t{2}}}); }, "16 MiB");
        check(!filesystem::exists(f.table()));
    });
    test("storage failures propagate", [] {
        Fixture f; write(f.root / "data", "not a directory");
        rejects([&] { f.run("INSERT INTO users VALUES (1,'A',1)"); }, "");
    });
    test("10000 rows round trip with scan filter", [] {
        Fixture f; string sql = "INSERT INTO users VALUES ";
        for (int i = 0; i < 10000; ++i) sql += (i ? ",(" : "(") + to_string(i) + ",'row'," + to_string(i % 100) + ")";
        check(f.run(sql).inserted == 10000);
        check(f.run("SELECT id FROM users WHERE age=42").rows.size() == 100);
        check(mini::formatResult(f.run("SELECT id FROM users WHERE id=9999")) == "id\n9999\n");
    });
    cout << passed << " passed, " << failed << " failed\n";
    return failed ? 1 : 0;
}
