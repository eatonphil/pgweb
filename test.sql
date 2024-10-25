DROP EXTENSION IF EXISTS pgweb;
CREATE EXTENSION pgweb;

DROP FUNCTION IF EXISTS handle_hello_world
CREATE FUNCTION handle_hello_world(params JSON) RETURNS TEXT AS $$
BEGIN
  RETURN 'Hello, ' || (params->'name') || '!';
END;
$$ LANGUAGE plpgsql;

SELECT pgweb.register_get('/hello', 'handle_hello_world');

SELECT pgweb.serve('localhost', 9090);
