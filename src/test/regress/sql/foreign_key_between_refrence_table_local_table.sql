CREATE SCHEMA fkey_reference_local_table;
SET search_path TO 'fkey_reference_local_table';

SET citus.shard_replication_factor to 1;

------------------------------------------------------------------------------------------------------
--- ALTER TABLE commands defining foreign key CONSTRAINT between local tables and reference tables ---
------------------------------------------------------------------------------------------------------

-------------------------------------------------------
-- * foreign key from local table to reference table --
-------------------------------------------------------

-- create test tables
CREATE TABLE local_table(l1 int);
CREATE TABLE reference_table(r1 int primary key);
SELECT create_reference_table('reference_table');

-- this should fail as reference table does not have a placement in coordinator
ALTER TABLE local_table ADD CONSTRAINT fkey_local_to_ref FOREIGN KEY(l1) REFERENCES reference_table(r1);

-- replicate reference table to coordinator
SET client_min_messages to ERROR;
SELECT 1 FROM master_add_node('localhost', :master_port, groupId => 0);
RESET client_min_messages;

-- We do not support "ALTER TABLE ADD CONSTRAINT foreign key from local table to
-- reference table" within the transaction block.
BEGIN;
  ALTER TABLE local_table ADD CONSTRAINT fkey_local_to_ref FOREIGN KEY(l1) REFERENCES reference_table(r1);
ROLLBACK;

-- We support ON DELETE CASCADE behaviour in "ALTER TABLE ADD CONSTRAINT foreign key from local table
-- to reference table" commands.
ALTER TABLE local_table ADD CONSTRAINT fkey_local_to_ref FOREIGN KEY(l1) REFERENCES reference_table(r1) ON DELETE CASCADE;

-- show that ON DELETE CASCADE works
INSERT INTO reference_table VALUES (11);
INSERT INTO local_table VALUES (11);
DELETE FROM reference_table WHERE r1=11;
SELECT count(*) FROM local_table;

-- show that we support DROP foreign key CONSTRAINT
ALTER TABLE local_table DROP CONSTRAINT fkey_local_to_ref;

-- We support ON UPDATE CASCADE behaviour in "ALTER TABLE ADD CONSTRAINT foreign key from local table
-- to reference table" commands.
ALTER TABLE local_table ADD CONSTRAINT fkey_local_to_ref FOREIGN KEY(l1) REFERENCES reference_table(r1) ON UPDATE CASCADE;

-- show that ON UPDATE CASCADE works
INSERT INTO reference_table VALUES (12);
INSERT INTO local_table VALUES (12);
UPDATE reference_table SET r1=13 WHERE r1=12;
SELECT * FROM local_table ORDER BY l1;

-- DROP foreign key CONSTRAINT for next commands
ALTER TABLE local_table DROP CONSTRAINT fkey_local_to_ref;

-- show that we are checking for foreign key CONSTRAINT validity while defining

INSERT INTO local_table VALUES (2);

-- this should fail
ALTER TABLE local_table ADD CONSTRAINT fkey_local_to_ref FOREIGN KEY(l1) REFERENCES reference_table(r1);

INSERT INTO reference_table VALUES (2);

-- this should work
ALTER TABLE local_table ADD CONSTRAINT fkey_local_to_ref FOREIGN KEY(l1) REFERENCES reference_table(r1);

-- show that we are checking for foreign key CONSTRAINT validity after defining

-- this should fail
INSERT INTO local_table VALUES (1);

INSERT INTO reference_table VALUES (1);

-- this should work
INSERT INTO local_table VALUES (1);

-- We support "ALTER TABLE DROP CONSTRAINT foreign key from local table to a reference table"
-- within the transaction block.
BEGIN;
  ALTER TABLE local_table DROP CONSTRAINT fkey_local_to_ref;
ROLLBACK;

-- Show that we DO NOT allow removing coordinator when we have a foreign key CONSTRAINT
-- from a local table to a reference table.
SELECT 1 FROM master_remove_node('localhost', :master_port);

-- show that DROP without cascade should error out
DROP TABLE reference_table;

-- DROP them at once
BEGIN;
  DROP TABLE reference_table CASCADE;
ROLLBACK;

-- DROP them at once
DROP TABLE reference_table, local_table;

-------------------------------------------------------
-- * foreign key from reference table to local table --
-------------------------------------------------------

-- create one reference table and one distributed table for next tests
-- also recreate local table
CREATE TABLE reference_table(r1 int primary key);
SELECT create_reference_table('reference_table');
CREATE TABLE distributed_table(d1 int primary key);
SELECT create_distributed_table('distributed_table', 'd1');
DROP TABLE local_table;
CREATE TABLE local_table(l1 int primary key);

-- chain the tables's foreign key CONSTRAINTs to each other (distributed -> reference -> local)
ALTER TABLE distributed_table ADD CONSTRAINT fkey_dist_to_ref FOREIGN KEY(d1) REFERENCES reference_table(r1);
ALTER TABLE reference_table ADD CONSTRAINT fkey_ref_to_local FOREIGN KEY(r1) REFERENCES local_table(l1);

INSERT INTO distributed_table VALUES (41);
INSERT INTO reference_table VALUES (41);
-- this should work
INSERT INTO local_table VALUES (41);

-- below should fail
DROP TABLE reference_table;

-- Below should be executed successfully as we handle the foreign key dependencies properly
-- when issueing DROP commands. (witohut deadlocks and with no weird errors etc.)
DROP TABLE local_table, reference_table, distributed_table;

-- create test tables
CREATE TABLE local_table(l1 int primary key);
CREATE TABLE reference_table(r1 int);
SELECT create_reference_table('reference_table');

-- remove coordinator node from pg_dist_node
SELECT 1 FROM master_remove_node('localhost', :master_port);

-- this should fail
ALTER TABLE reference_table ADD CONSTRAINT fkey_ref_to_local FOREIGN KEY(r1) REFERENCES local_table(l1);

-- We do not support "ALTER TABLE ADD CONSTRAINT foreign key from reference table to local table"
-- within the transaction block.
BEGIN;
  ALTER TABLE reference_table ADD CONSTRAINT fkey_ref_to_local FOREIGN KEY(r1) REFERENCES local_table(l1);
COMMIt;

-- We DO NOT support ON UPDATE/DELETE CASCADE behaviour in "ALTER TABLE ADD CONSTRAINT foreign key
-- from reference table to local table" commands, below two should error out.
ALTER TABLE reference_table ADD CONSTRAINT fkey_ref_to_local FOREIGN KEY(r1) REFERENCES local_table(l1) ON UPDATE CASCADE;
ALTER TABLE reference_table ADD CONSTRAINT fkey_ref_to_local FOREIGN KEY(r1) REFERENCES local_table(l1) ON DELETE CASCADE;

-- replicate reference table to coordinator
SET client_min_messages to ERROR;
SELECT 1 FROM master_add_node('localhost', :master_port, groupId => 0);
RESET client_min_messages;

-- show that we are checking for foreign key CONSTRAINT validity while defining

INSERT INTO reference_table VALUES (3);

-- this should fail
ALTER TABLE reference_table ADD CONSTRAINT fkey_ref_to_local FOREIGN KEY(r1) REFERENCES local_table(l1);

INSERT INTO local_table VALUES (3);

-- We DO NOT support ON DELETE/UPDATE CASCADE behaviour in "ALTER TABLE ADD CONSTRAINT foreign key
-- from reference table to local table" commands
ALTER TABLE reference_table ADD CONSTRAINT fkey_ref_to_local FOREIGN KEY(r1) REFERENCES local_table(l1) ON DELETE CASCADE;
ALTER TABLE reference_table ADD CONSTRAINT fkey_ref_to_local FOREIGN KEY(r1) REFERENCES local_table(l1) ON UPDATE CASCADE;

-- this should work
ALTER TABLE reference_table ADD CONSTRAINT fkey_ref_to_local FOREIGN KEY(r1) REFERENCES local_table(l1);

-- show that we are checking for foreign key CONSTRAINT validity after defining

-- this should fail
INSERT INTO reference_table VALUES (4);

INSERT INTO local_table VALUES (4);

-- this should work
INSERT INTO reference_table VALUES (4);

-- We do support "ALTER TABLE DROP CONSTRAINT foreign key from reference table
-- to local table" within a transaction block.
BEGIN;
  ALTER TABLE reference_table DROP CONSTRAINT fkey_ref_to_local;
COMMIt;

-- Show that we DO allow removing coordinator when we have a foreign key CONSTRAINT
-- from a reference table to a local table.
SELECT 1 FROM master_remove_node('localhost', :master_port);

ALTER TABLE reference_table ADD CONSTRAINT fkey_ref_to_local FOREIGN KEY(r1) REFERENCES local_table(l1);

-- show that we support DROP CONSTRAINT
BEGIN;
  ALTER TABLE reference_table DROP CONSTRAINT fkey_ref_to_local;
ROLLBACK;

-- show that DROP table errors out as expected
DROP TABLE local_table;

-- this should work
BEGIN;
  DROP TABLE local_table CASCADE;
ROLLBACK;

-- this should also work
DROP TABLE local_table, reference_table;

-------------------------------------------------------------
-- * foreign key between reference tables and local tables --
-------------------------------------------------------------

-- Show that we can ADD foreign key CONSTRAINT from/to a reference table that
-- needs to be escaped.
-- That is to see if we convert reference table relation to its local placement
-- relation in such ALTER TABLE ADD CONSTRAINT foreign key commands properly

CREATE TABLE local_table(l1 int primary key);
CREATE TABLE "reference'table"(r1 int primary key);
SELECT create_reference_table('reference''table');

-- replicate reference table to coordinator
SET client_min_messages to ERROR;
SELECT 1 FROM master_add_node('localhost', :master_port, groupId => 0);
RESET client_min_messages;

-- these should work
ALTER TABLE local_table ADD CONSTRAINT fkey_local_to_ref FOREIGN KEY(l1) REFERENCES "reference'table"(r1);
INSERT INTO "reference'table" VALUES (21);
INSERT INTO local_table VALUES (21);

-- This should fail with an appropriate error message like we do for reference
-- tables that do not need to be escaped
INSERT INTO local_table VALUES (22);

-- DROP CONSTRAINT for next commands
ALTER TABLE local_table DROP CONSTRAINT fkey_local_to_ref;

-- these should also work
ALTER TABLE "reference'table" ADD CONSTRAINT fkey_ref_to_local FOREIGN KEY(r1) REFERENCES local_table(l1);
INSERT INTO local_table VALUES (23);
INSERT INTO "reference'table" VALUES (23);

-- This should fail with an appropriate error message like we do for reference tables that
-- do not need to be escaped
INSERT INTO local_table VALUES (24);

-- DROP tables finally
DROP TABLE local_table, "reference'table";

-------------------------------------------------------------------------------------------------------
--- CREATE TABLE commands defining foreign key CONSTRAINT between local tables and reference tables ---
-------------------------------------------------------------------------------------------------------

-- TODO: this section is not implemented yet, ignore below

-- remove master node from pg_dist_node for next tests to show that
-- behaviour does not need us to add coordinator to pg_dist_node priorly,
-- as it is not implemented in the ideal way (for now)
SELECT 1 FROM master_remove_node('localhost', :master_port);

-- create tables
CREATE TABLE reference_table (r1 int);
CREATE TABLE local_table (l1 int REFERENCES reference_table(r1));

-- actually, we did not implement upgrading "a local table referenced by another local table"
-- to a reference table yet -in an ideal way-. But it should work producing a warning
SELECT create_reference_table("reference_table");

-- show that we are checking for foreign key CONSTRAINT validity after defining

-- this should fail
INSERT INTO local_table VALUES (31);

INSERT INTO reference_table VALUES (31);

-- this should work
INSERT INTO local_table VALUES (31);

-- that amount of test for CREATE TABLE commands defining an foreign key CONSTRAINT
-- from a local table to a reference table is sufficient it is already tested
-- in some other regression tests already

-- DROP tables finally
DROP TABLE local_table;
DROP TABLE "reference'table";

-- create tables
CREATE TABLE local_table (l1 int);
CREATE TABLE reference_table (r1 int REFERENCES local_table(l1));

-- we did not implement upgrading "a local table referencing to another
-- local table" to a reference table yet.
-- this should fail
SELECT create_reference_table("reference_table");

-- finalize the test, clear the schema created for this test --
DROP SCHEMA fkey_reference_local_table;
