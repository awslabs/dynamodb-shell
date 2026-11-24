-- Copyright 2026 ddbsh reimplementation contributors.
-- SPDX-License-Identifier: Apache-2.0
--
-- ddbsh-fmt-load.sql: the seed for the formats family (families/formats.txt).
-- One partition (pk = 1), so a Query returns the rows in rk order and the
-- goldens need no sort. The rows have different attributes, so CSV and the
-- table show the union of columns. rk = 4 (binary and the three set types,
-- which have no ddbsh literal) is written by ddbsh-put-item.
create table ddbsh_fmt ( pk number, rk number ) primary key ( pk hash, rk range );
insert into ddbsh_fmt ( pk, rk, a, b, c, d ) values ( 1, 1, "hello", 42, TRUE, NULL );
insert into ddbsh_fmt ( pk, rk, a, e, f ) values ( 1, 2, "say ""hi"", ok", 12345678901234567890.123456789, "C:\dir" );
insert into ddbsh_fmt ( pk, rk, l, m, u ) values ( 1, 3, [ 1, "two", [ 3 ], {} ], { y: { z: "deep" }, x: 1 }, "héllo wörld" );
