-- pgweb--0.0.1.sql

-- Create the extension schema
CREATE SCHEMA pgweb;

-- Registers a route with a Postgres function handler.
-- Example:
--   DROP FUNCTION IF EXISTS handle_hello_world
--   CREATE FUNCTION handle_hello_world(params HSTORE) RETURNS TEXT AS $$
--   BEGIN
--     RETURN 'Hello, ' || params['name'] || '!';
--   END;
--   $$ LANGUAGE plpgsql;
--
--   SELECT pgweb.register_get('/hello', 'handle_hello_world');
CREATE OR REPLACE FUNCTION pgweb.register_get(TEXT, TEXT)
RETURNS VOID AS 'pgweb', 'pgweb_register_get'
LANGUAGE C STRICT;
GRANT EXECUTE ON FUNCTION pgweb.register_get(TEXT, TEXT) TO PUBLIC;

-- Starts the web server at the address and port.
-- Example:
--   SELECT pgweb.serve('localhost', 9090);
CREATE OR REPLACE FUNCTION pgweb.serve(TEXT, INT)
RETURNS VOID AS 'pgweb', 'pgweb_serve'
LANGUAGE C STRICT;
GRANT EXECUTE ON FUNCTION pgweb.serve(TEXT, INT) TO PUBLIC;

-- Example:
--  $ curl localhost:9090/hello?name=Phil
--  Hello, Phil!
