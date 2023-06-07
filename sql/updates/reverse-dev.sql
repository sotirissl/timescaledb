-- API changes related to hypertable generalization
DROP FUNCTION IF EXISTS timescaledb_experimental.create_hypertable;
DROP FUNCTION IF EXISTS timescaledb_experimental.add_dimension;
DROP FUNCTION IF EXISTS timescaledb_experimental.set_partitioning_interval;
