-- Copyright 2026 ddbsh reimplementation contributors.
-- SPDX-License-Identifier: Apache-2.0
--
-- Seed for the vector-filters golden family (P13 T1): SearchConditionExpression
-- operators on vector-index inline filters.
--
-- Two vector indexes on the same 2-dimension vector `v`:
--   fidx  search key ( tenant HASH, price FILTER, rating FILTER, brand FILTER )
--   widx  search key ( price FILTER, rating FILTER, brand FILTER ), no HASH, so
--         a search covers the whole index.
-- Item `id` N has v = [N, 0]. Every query searches from [0, 0] with euclidean
-- distance, so item N scores exactly N and no two scores tie. Items 6 and 12
-- carry no `brand`, which pins how `<>`, NOT and attribute_not_exists treat a
-- missing inline-filter attribute.
--
--   id  tenant  price  rating  brand
--    1  t1      10     1       acme
--    2  t1      20     2       bravo
--    3  t1      30     3       cirque
--    4  t1      40     4       acme
--    5  t1      50     5       bravo
--    6  t1      60     1       -
--    7  t2      15     2       acme
--    8  t2      25     3       bravo
--    9  t2      35     4       cirque
--   10  t2      45     5       acme
--   11  t2      55     1       bravo
--   12  t2      65     2       -
--
-- The CREATE waits for both vector indexes to be ACTIVE (~30s), as the vector
-- family's seeds do.
create table ddbsh_vec_f ( id number, tenant string, price number, rating number, brand string ) primary key ( id hash ) vsi ( fidx on v dimensions 2 distance euclidean search key ( tenant hash, price filter, rating filter, brand filter ) projecting all, widx on v dimensions 2 distance euclidean search key ( price filter, rating filter, brand filter ) projecting all );
insert into ddbsh_vec_f (id, tenant, price, rating, brand, v) values (1, "t1", 10, 1, "acme", [1, 0]);
insert into ddbsh_vec_f (id, tenant, price, rating, brand, v) values (2, "t1", 20, 2, "bravo", [2, 0]);
insert into ddbsh_vec_f (id, tenant, price, rating, brand, v) values (3, "t1", 30, 3, "cirque", [3, 0]);
insert into ddbsh_vec_f (id, tenant, price, rating, brand, v) values (4, "t1", 40, 4, "acme", [4, 0]);
insert into ddbsh_vec_f (id, tenant, price, rating, brand, v) values (5, "t1", 50, 5, "bravo", [5, 0]);
insert into ddbsh_vec_f (id, tenant, price, rating, v) values (6, "t1", 60, 1, [6, 0]);
insert into ddbsh_vec_f (id, tenant, price, rating, brand, v) values (7, "t2", 15, 2, "acme", [7, 0]);
insert into ddbsh_vec_f (id, tenant, price, rating, brand, v) values (8, "t2", 25, 3, "bravo", [8, 0]);
insert into ddbsh_vec_f (id, tenant, price, rating, brand, v) values (9, "t2", 35, 4, "cirque", [9, 0]);
insert into ddbsh_vec_f (id, tenant, price, rating, brand, v) values (10, "t2", 45, 5, "acme", [10, 0]);
insert into ddbsh_vec_f (id, tenant, price, rating, brand, v) values (11, "t2", 55, 1, "bravo", [11, 0]);
insert into ddbsh_vec_f (id, tenant, price, rating, v) values (12, "t2", 65, 2, [12, 0]);
