#!/usr/bin/env python3
"""Calls MySQL's built-in functions once each through the aios binary (no server, no build) and says which ASQL does
not know and which answer differently from what MySQL documents.

    python3 tools/asql-functions/probe.py [path/to/aios]

Each entry is an expression and MySQL's answer for it (None: only that ASQL knows the function). The answers are
MySQL 5.7 / MariaDB 10 behaviour, for fixed inputs (no clock, no randomness).
"""
import os, re, subprocess, sys, tempfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
AIOS = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, 'build', 'aios')

CASES = [
    # strings
    ("CONCAT('a', 1, 'b')", 'a1b'), ("CONCAT('a', NULL)", 'NULL'), ("CONCAT_WS('-', 'a', NULL, 'b')", 'a-b'),
    ("LENGTH('héllo')", '6'), ("CHAR_LENGTH('héllo')", '5'), ("CHARACTER_LENGTH('ab')", '2'), ("OCTET_LENGTH('ab')", '2'),
    ("UPPER('abc')", 'ABC'), ("UCASE('abc')", 'ABC'), ("LOWER('ABC')", 'abc'), ("LCASE('ABC')", 'abc'),
    ("SUBSTRING('abcdef', 2, 3)", 'bcd'), ("SUBSTRING('abcdef', -2)", 'ef'), ("SUBSTR('abcdef', 3)", 'cdef'), ("MID('abcdef', 2, 2)", 'bc'),
    ("SUBSTRING_INDEX('a.b.c', '.', 2)", 'a.b'), ("SUBSTRING_INDEX('a.b.c', '.', -1)", 'c'),
    ("LEFT('abcdef', 2)", 'ab'), ("RIGHT('abcdef', 2)", 'ef'), ("TRIM('  x  ')", 'x'), ("LTRIM('  x')", 'x'), ("RTRIM('x  ')", 'x'),
    ("TRIM(LEADING '0' FROM '00120')", '120'), ("TRIM(BOTH 'x' FROM 'xxaxx')", 'a'), ("TRIM(TRAILING '.' FROM 'a..')", 'a'),
    ("REPLACE('aXbX', 'X', 'y')", 'ayby'), ("REVERSE('abc')", 'cba'), ("REPEAT('ab', 3)", 'ababab'), ("SPACE(3)", '   '),
    ("LPAD('5', 3, '0')", '005'), ("RPAD('5', 3, '0')", '500'), ("LPAD('abcd', 2, '0')", 'ab'),
    ("INSTR('foobar', 'bar')", '4'), ("LOCATE('bar', 'foobarbar')", '4'), ("LOCATE('bar', 'foobarbar', 5)", '7'), ("POSITION('b' IN 'abc')", '2'),
    ("FIND_IN_SET('b', 'a,b,c')", '2'), ("FIND_IN_SET('z', 'a,b,c')", '0'), ("FIELD('b', 'a', 'b', 'c')", '2'), ("ELT(2, 'a', 'b', 'c')", 'b'),
    ("INSERT('Quadratic', 3, 4, 'What')", 'QuWhattic'), ("STRCMP('a', 'b')", '-1'), ("ASCII('A')", '65'), ("ORD('A')", '65'), ("CHAR(65, 66)", 'AB'),
    ("HEX('abc')", '616263'), ("HEX(255)", 'FF'), ("UNHEX('616263')", 'abc'), ("BIN(5)", '101'), ("OCT(8)", '10'), ("CONV('ff', 16, 10)", '255'),
    ("QUOTE('a''b')", "'a\\'b'"), ("SOUNDEX('Robert')", 'R163'), ("FORMAT(1234567.891, 2)", '1,234,567.89'), ("FORMAT(0.5, 0)", '1'),
    ("MD5('a')", '0cc175b9c0f1b6a831c399e269772661'), ("SHA1('a')", '86f7e437faa5a7fce15d1ddcb9eaeaea377667b8'),
    ("SHA2('a', 256)", 'ca978112ca1bbdcafac231b39a23dc4da786eff8147c4e72b9807785afee48bb'), ("CRC32('a')", '3904355907'),
    ("TO_BASE64('abc')", 'YWJj'), ("FROM_BASE64('YWJj')", 'abc'), ("'abc' LIKE 'a%'", '1'), ("'abc' REGEXP '^a.c$'", '1'),
    ("REGEXP_REPLACE('a1b22', '[0-9]+', '#')", 'a#b#'), ("REGEXP_SUBSTR('ab123cd', '[0-9]+')", '123'), ("REGEXP_INSTR('ab123', '[0-9]')", '3'),
    ("CAST('12abc' AS UNSIGNED)", '12'), ("CAST(-1 AS UNSIGNED)", '18446744073709551615'), ("CAST('2026-03-04' AS DATE)", '2026-03-04'),
    ("CAST(1.5 AS SIGNED)", '2'), ("CAST(12.345 AS DECIMAL(10,2))", '12.35'), ("CAST(5 AS CHAR)", '5'), ("CONVERT('5', SIGNED)", '5'),
    ("CONVERT('abc' USING utf8)", 'abc'), ("BINARY 'a' = 'A'", '0'), ("'a' = 'A'", '1'), ("'a ' = 'a'", '1'),
    # numbers
    ("ABS(-3)", '3'), ("CEIL(1.2)", '2'), ("CEILING(-1.2)", '-1'), ("FLOOR(-1.2)", '-2'), ("ROUND(2.5)", '3'), ("ROUND(-2.5)", '-3'),
    ("ROUND(1.2345, 2)", '1.23'), ("ROUND(1234.5, -2)", '1200'), ("TRUNCATE(1.999, 1)", '1.9'), ("TRUNCATE(-1.999, 0)", '-1'),
    ("MOD(10, 3)", '1'), ("10 % 3", '1'), ("-7 MOD 3", '-1'), ("10 DIV 3", '3'), ("7 / 2", '3.5000'), ("1 / 0", 'NULL'),
    ("POW(2, 10)", '1024'), ("POWER(2, 3)", '8'), ("SQRT(16)", '4'), ("EXP(0)", '1'), ("LN(1)", '0'), ("LOG(1)", '0'), ("LOG10(100)", '2'),
    ("LOG2(8)", '3'), ("LOG(2, 8)", '3'), ("SIGN(-5)", '-1'), ("PI()", '3.141593'), ("GREATEST(1, 5, 3)", '5'), ("LEAST(1, 5, 3)", '1'),
    ("GREATEST(1, NULL)", 'NULL'), ("RADIANS(180)", '3.141592653589793'), ("DEGREES(PI())", '180'), ("SIN(0)", '0'), ("COS(0)", '1'),
    ("TAN(0)", '0'), ("ATAN(0)", '0'), ("ATAN2(0, 1)", '0'), ("ASIN(0)", '0'), ("ACOS(1)", '0'), ("COT(1)", '0.6420926159343306'),
    ("CRC32('')", '0'), ("BIT_COUNT(7)", '3'), ("5 & 3", '1'), ("5 | 3", '7'), ("5 ^ 3", '6'), ("1 << 3", '8'), ("~0", '18446744073709551615'),
    # control flow / null
    ("IF(1 > 0, 'y', 'n')", 'y'), ("IFNULL(NULL, 'd')", 'd'), ("NULLIF(1, 1)", 'NULL'), ("COALESCE(NULL, NULL, 3)", '3'),
    ("CASE 2 WHEN 1 THEN 'a' WHEN 2 THEN 'b' ELSE 'c' END", 'b'), ("ISNULL(NULL)", '1'), ("NULL <=> NULL", '1'), ("1 IS TRUE", '1'),
    ("INTERVAL(5, 1, 3, 7)", '2'), ("1 BETWEEN 0 AND 2", '1'), ("3 IN (1, 2, 3)", '1'),
    # dates (fixed inputs)
    ("DATE('2026-03-04 05:06:07')", '2026-03-04'), ("TIME('2026-03-04 05:06:07')", '05:06:07'), ("YEAR('2026-03-04')", '2026'),
    ("MONTH('2026-03-04')", '3'), ("DAY('2026-03-04')", '4'), ("DAYOFMONTH('2026-03-04')", '4'), ("HOUR('05:06:07')", '5'),
    ("MINUTE('05:06:07')", '6'), ("SECOND('05:06:07')", '7'), ("QUARTER('2026-08-01')", '3'), ("WEEK('2026-03-04')", '9'),
    ("WEEK('2026-03-04', 1)", '10'), ("YEARWEEK('2026-03-04')", '202609'), ("WEEKDAY('2026-03-04')", '2'), ("DAYOFWEEK('2026-03-04')", '4'),
    ("DAYOFYEAR('2026-03-04')", '63'), ("DAYNAME('2026-03-04')", 'Wednesday'), ("MONTHNAME('2026-03-04')", 'March'),
    ("LAST_DAY('2026-02-10')", '2026-02-28'), ("DATE_ADD('2026-01-31', INTERVAL 1 MONTH)", '2026-02-28'),
    ("DATE_SUB('2026-03-01', INTERVAL 1 DAY)", '2026-02-28'), ("ADDDATE('2026-03-01', 3)", '2026-03-04'), ("SUBDATE('2026-03-01', 1)", '2026-02-28'),
    ("'2026-03-01' + INTERVAL 2 DAY", '2026-03-03'), ("DATE_ADD('2026-03-01 10:00:00', INTERVAL 90 MINUTE)", '2026-03-01 11:30:00'),
    ("DATEDIFF('2026-03-04', '2026-02-27')", '5'), ("TIMESTAMPDIFF(MONTH, '2026-01-15', '2026-03-14')", '1'), ("TIMESTAMPDIFF(DAY, '2026-03-01', '2026-03-04 12:00:00')", '3'),
    ("TIMESTAMPADD(DAY, 2, '2026-03-01')", '2026-03-03'), ("TIMEDIFF('10:00:00', '08:30:00')", '01:30:00'), ("ADDTIME('10:00:00', '01:30:00')", '11:30:00'),
    ("SUBTIME('10:00:00', '00:30:00')", '09:30:00'), ("TIME_TO_SEC('01:00:01')", '3601'), ("SEC_TO_TIME(3601)", '01:00:01'),
    ("TO_DAYS('2026-03-04')", '740044'), ("FROM_DAYS(740044)", '2026-03-04'), ("MAKEDATE(2026, 63)", '2026-03-04'), ("MAKETIME(5, 6, 7)", '05:06:07'),
    ("DATE_FORMAT('2026-03-04 05:06:07', '%Y-%m-%d %H:%i:%s')", '2026-03-04 05:06:07'), ("DATE_FORMAT('2026-03-04', '%W %M %e %y %b %a %j')", 'Wednesday March 4 26 Mar Wed 063'),
    ("DATE_FORMAT('2026-03-04 15:06:07', '%h %p %r %T')", '03 PM 03:06:07 PM 15:06:07'), ("STR_TO_DATE('04/03/2026', '%d/%m/%Y')", '2026-03-04'),
    ("STR_TO_DATE('2026-03-04 05:06', '%Y-%m-%d %H:%i')", '2026-03-04 05:06:00'), ("TIME_FORMAT('15:06:07', '%H:%i')", '15:06'),
    ("UNIX_TIMESTAMP('1970-01-02 00:00:00') - UNIX_TIMESTAMP('1970-01-01 00:00:00')", '86400'), ("PERIOD_DIFF(202603, 202512)", '3'),
    ("PERIOD_ADD(202512, 2)", '202602'), ("EXTRACT(YEAR_MONTH FROM '2026-03-04')", '202603'), ("EXTRACT(DAY FROM '2026-03-04')", '4'),
    ("DATE('2026-02-30')", 'NULL'), ("STR_TO_DATE('2026-03-04', '%Y-%m-%d') + INTERVAL 1 DAY", '2026-03-05'),
    ("CONVERT_TZ('2026-03-04 12:00:00', '+00:00', '-05:00')", '2026-03-04 07:00:00'), ("UTC_DATE() IS NOT NULL", '1'), ("CURDATE() = CURRENT_DATE", '1'),
    ("NOW() >= CURDATE()", '1'), ("SYSDATE() IS NOT NULL", '1'), ("CURTIME() IS NOT NULL", '1'), ("LOCALTIMESTAMP() IS NOT NULL", '1'),
    # json
    ("JSON_EXTRACT('{\"a\": {\"b\": 5}}', '$.a.b')", '5'), ("JSON_UNQUOTE(JSON_EXTRACT('{\"a\": \"x\"}', '$.a'))", 'x'),
    ("JSON_OBJECT('a', 1)", '{"a": 1}'), ("JSON_ARRAY(1, 'a')", '[1, "a"]'), ("JSON_LENGTH('[1, 2, 3]')", '3'), ("JSON_VALID('{}')", '1'),
    ("JSON_CONTAINS('[1, 2]', '2')", '1'), ("JSON_KEYS('{\"a\": 1, \"b\": 2}')", '["a", "b"]'), ("JSON_SET('{\"a\": 1}', '$.b', 2)", '{"a": 1, "b": 2}'),
    ("JSON_SEARCH('[\"x\", \"y\"]', 'one', 'y')", '"$[1]"'), ("JSON_TYPE('[]')", 'ARRAY'), ("JSON_QUOTE('a')", '"a"'),
    # misc
    ("COALESCE(NULL, 'x')", 'x'), ("DATABASE() IS NOT NULL", '1'), ("UUID() IS NOT NULL", '1'), ("INET_ATON('10.0.0.1')", '167772161'),
    ("INET_NTOA(167772161)", '10.0.0.1'), ("BIT_LENGTH('a')", '8'), ("ROW_COUNT() IS NOT NULL", '1'), ("FOUND_ROWS() IS NOT NULL", '1'),
    ("GET_LOCK('x', 0)", '1'), ("RELEASE_LOCK('x')", '1'), ("IS_FREE_LOCK('zz')", '1'), ("SLEEP(0)", '0'), ("BENCHMARK(1, 1)", '0'),
    ("CHARSET('a') IS NOT NULL", '1'), ("COLLATION('a') IS NOT NULL", '1'), ("VERSION() IS NOT NULL", '1'), ("CONNECTION_ID() > 0", '1'),
    ("ENCODE('a', 'k') IS NOT NULL", None), ("AES_DECRYPT(AES_ENCRYPT('abc', 'k'), 'k')", 'abc'), ("COMPRESS('a') IS NOT NULL", '1'),
    ("NAME_CONST('n', 5)", '5'), ("ANY_VALUE(3)", '3'), ("VALUES(1)", None), ("DEFAULT(1)", None),
]


def main():
    tmp = tempfile.mkdtemp()
    cmds = []
    for expr, _ in CASES:
        cmds.append('sql query "SELECT ' + expr.replace('\\', '\\\\').replace('"', '\\"') + ' AS r"')
    out = subprocess.run([AIOS, '--root', os.path.join(tmp, 'root'), 'exec', *cmds], capture_output=True, text=True,
                         env={**os.environ, 'AIOS_NO_ACCEL': '1'}).stdout
    blocks = re.split(r'\[SQL (RESULT|ERROR)\]', out)[1:]
    results = []
    for i in range(0, len(blocks), 2):
        kind, body = blocks[i], blocks[i + 1]
        if kind == 'ERROR':
            results.append(('error', body.strip().splitlines()[0] if body.strip() else ''))
        else:
            rows = [l for l in body.splitlines() if l.startswith('|')]
            val = rows[1].strip().strip('|').strip() if len(rows) > 1 else ''
            results.append(('ok', val))
    unknown, wrong, errors = [], [], []
    for (expr, want), (kind, got) in zip(CASES, results):
        if kind == 'error':
            (unknown if 'unknown function' in got else errors).append((expr, got))
        elif want is not None and got != want:
            try:
                if abs(float(got) - float(want)) < 1e-6: continue
            except ValueError:
                pass
            wrong.append((expr, got, want))
    if len(results) != len(CASES): print(f'note: {len(results)} answers for {len(CASES)} expressions')
    print(f'{len(CASES)} expressions: {len(unknown)} unknown, {len(errors)} other errors, {len(wrong)} answers that differ from MySQL\n')
    for expr, got in unknown: print(f'UNKNOWN  {expr}')
    for expr, got in errors: print(f'ERROR    {expr}  ->  {got}')
    for expr, got, want in wrong: print(f'DIFFERS  {expr}  ->  {got!r}, MySQL {want!r}')


if __name__ == '__main__':
    main()
