CREATE SCHEMA adaptive_executor;
SET search_path TO adaptive_executor;

SET citus.task_executor_type to 'adaptive';
SET citus.shard_replication_factor to 1;
SET citus.enable_repartition_joins TO true;

CREATE TABLE ab(a int, b int);
SELECT create_distributed_table('ab', 'a');
INSERT INTO ab SELECT *,* FROM generate_series(1,10);

SELECT COUNT(*) FROM ab k, ab l 
WHERE k.a = l.b; 

SELECT COUNT(*) FROM ab k, ab l, ab m, ab t 
WHERE k.a = l.b AND k.a = m.b AND t.b = l.a; 

DROP SCHEMA adaptive_executor CASCADE;
