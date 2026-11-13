-- UTF-8 SQL script. IDs are supplied explicitly in Phase 1.
INSERT INTO users (id, name, age) VALUES
    (1, '김정글', 25),
    (2, 'O''Brien', 31),
    (3, 'Alice, developer', 22);

SELECT * FROM users;
SELECT name, age FROM users WHERE age >= 25 AND id <> 2;
SELECT id, name FROM users WHERE id = 2;

INSERT INTO products VALUES (1, '키보드', 120000);
SELECT name, price FROM products WHERE price < 150000;
