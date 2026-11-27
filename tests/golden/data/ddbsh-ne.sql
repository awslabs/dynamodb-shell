-- Copyright 2026 ddbsh reimplementation contributors.
-- SPDX-License-Identifier: Apache-2.0
--
-- Seed for the not-equal golden family (P15, B6, D-P15-8): `<>`, `!=` and
-- `NOT x = v` on a sort key, which plan as two Queries (`< v`, then `> v`).
--
-- ddbsh_ne: pk N HASH, sk N RANGE; LSI ne_lsi (pk HASH, l RANGE); GSI ne_gsi
-- (g HASH, h RANGE); composite GSI ne_cgsi ((g, q) HASH, (h, r) RANGE).
--
--   pk  sk  g  q  h  r  l  x
--    1   1  a  z  1  1  9  10
--    1   2  a  z  2  2  8  20
--    1   3  a  z  3  3  7  30
--    1   4  a  z  4  4  6  --   (no x)
--    1   5  a  z  5  5  5  50
--    1   6  a  z  6  6  4  60
--    1   7  a  z  7  7  3  70
--    1   8  a  z  8  8  2  80
--    1   9  a  z  9  9  1  90
--    2   3  b  z  3  3  7  30
--
-- ddbsh_ne_w: the same key schema, no indexes, for the write goldens. Each
-- write golden reseeds it with REPLACE (pk 1, sk 1..9 and pk 2, sk 3, no other
-- attributes) so it does not depend on the order the tests run in.
--
-- ddbsh_ne_s: pk N HASH, sk S RANGE: (1, apple), (1, banana), (1, cherry).
create table ddbsh_ne ( pk number, sk number, g string, h number, l number, q string, r number )
    primary key ( pk hash, sk range )
    gsi ( ne_gsi on ( g hash, h range ) projecting all,
          ne_cgsi on ( (g, q) hash, (h, r) range ) projecting all )
    lsi ( ne_lsi on ( pk hash, l range ) projecting all );
insert into ddbsh_ne ( pk, sk, g, q, h, r, l, x ) values
    ( 1, 1, "a", "z", 1, 1, 9, 10 ),
    ( 1, 2, "a", "z", 2, 2, 8, 20 ),
    ( 1, 3, "a", "z", 3, 3, 7, 30 );
insert into ddbsh_ne ( pk, sk, g, q, h, r, l ) values
    ( 1, 4, "a", "z", 4, 4, 6 );
insert into ddbsh_ne ( pk, sk, g, q, h, r, l, x ) values
    ( 1, 5, "a", "z", 5, 5, 5, 50 ),
    ( 1, 6, "a", "z", 6, 6, 4, 60 ),
    ( 1, 7, "a", "z", 7, 7, 3, 70 ),
    ( 1, 8, "a", "z", 8, 8, 2, 80 ),
    ( 1, 9, "a", "z", 9, 9, 1, 90 ),
    ( 2, 3, "b", "z", 3, 3, 7, 30 );
create table ddbsh_ne_w ( pk number, sk number ) primary key ( pk hash, sk range );
insert into ddbsh_ne_w ( pk, sk ) values
    ( 1, 1 ), ( 1, 2 ), ( 1, 3 ), ( 1, 4 ), ( 1, 5 ), ( 1, 6 ), ( 1, 7 ), ( 1, 8 ), ( 1, 9 ),
    ( 2, 3 );
create table ddbsh_ne_s ( pk number, sk string ) primary key ( pk hash, sk range );
insert into ddbsh_ne_s ( pk, sk ) values ( 1, "apple" ), ( 1, "banana" ), ( 1, "cherry" );
