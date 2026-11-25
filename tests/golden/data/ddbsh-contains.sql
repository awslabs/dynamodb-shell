-- Copyright 2026 ddbsh reimplementation contributors.
-- SPDX-License-Identifier: Apache-2.0
--
-- Seed for the contains golden family (P13 T2, issue #20): CONTAINS with a
-- NULL, number or boolean operand. The number-set items (pk 4 and 5) have no
-- ddbsh literal, so the family writes them with ddbsh-put-item.
--
--   pk  l                 s
--    1  [1, NULL, "x"]    hello
--    2  ["a", "b"]        world
--    3  [TRUE, 2]         true
--    6  [3.5, FALSE]      six
create table ddbsh_contains ( pk number ) primary key ( pk hash );
insert into ddbsh_contains (pk, l, s) values (1, [1, NULL, "x"], "hello");
insert into ddbsh_contains (pk, l, s) values (2, ["a", "b"], "world");
insert into ddbsh_contains (pk, l, s) values (3, [TRUE, 2], "true");
insert into ddbsh_contains (pk, l, s) values (6, [3.5, FALSE], "six");
