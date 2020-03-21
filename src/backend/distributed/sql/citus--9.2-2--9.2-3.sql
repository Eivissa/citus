-- Enable SSL to encrypt all trafic by default

-- create temporary UDF that has the power to change settings within postgres and drop it
-- after ssl has been setup.

-- This exact same function has already been run for clusters that were
-- originally created with Citus 8.1.0 or later. However, this is not
-- necessarily the case for clusters originally created with a version before
-- 8.1.0. That's why we run it again here. If SSL is already set up this is a
-- no-op, else this sets up self signed certificates and enables SSL. There
-- will still be a difference between clusters from before 8.1.0 and clusters
-- after 8.1.0. This difference is that for old clusters citus.node_conninfo
-- will be "sslmode=prefer", and for new clusters it will be "sslmode=require".
CREATE FUNCTION citus_setup_ssl()
    RETURNS void
    LANGUAGE C STRICT
    AS 'MODULE_PATHNAME', $$citus_setup_ssl$$;

DO LANGUAGE plpgsql
$$
BEGIN
    -- setup ssl when postgres is OpenSSL-enabled
    IF current_setting('ssl_ciphers') != 'none' THEN
        PERFORM citus_setup_ssl();
    END IF;
END;
$$;

DROP FUNCTION citus_setup_ssl();
