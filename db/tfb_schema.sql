-- TechEmpower Framework Benchmarks schema (PostgreSQL).
-- World: 10000 rows, id 1..10000, randomNumber 1..10000.
-- Fortune: the canonical 12 rows (incl. the XSS and UTF-8 entries that exercise
-- the /fortunes HTML-escaping and ordering rules).

DROP TABLE IF EXISTS World;
CREATE TABLE World (
    id           integer NOT NULL PRIMARY KEY,
    randomNumber integer NOT NULL DEFAULT 0
);
INSERT INTO World (id, randomNumber)
    SELECT x.id, floor(random() * 10000) + 1
    FROM generate_series(1, 10000) AS x(id);

DROP TABLE IF EXISTS Fortune;
CREATE TABLE Fortune (
    id      integer NOT NULL PRIMARY KEY,
    message varchar(2048) NOT NULL
);
INSERT INTO Fortune (id, message) VALUES
    (1,  'fortune: No such file or directory'),
    (2,  'A computer scientist is someone who fixes things that aren''t broken.'),
    (3,  'After enough decimal places, nobody gives a damn.'),
    (4,  'A bad random number generator: 1, 1, 1, 1, 1, 4.33e+67, 1, 1, 1'),
    (5,  'A computer program does what you tell it to do, not what you want it to do.'),
    (6,  'Emacs is a nice operating system, but I prefer UNIX. — Tom Christaensen'),
    (7,  'Any program that runs right is obsolete.'),
    (8,  'A list is only as strong as its weakest link. — Donald Knuth'),
    (9,  'Feature: A bug with seniority.'),
    (10, 'Computers make very fast, very accurate mistakes.'),
    (11, '<script>alert("This should not be displayed in a browser alert box.");</script>'),
    (12, 'フレームワークのベンチマーク');
