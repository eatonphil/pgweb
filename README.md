First build Postgres locally.

```console
$ cd ~
$ git clone https://github.com/postgres/postgres
$ cd postgres
$ git checkout REL_16_STABLE
$ ./configure --prefix=$(pwd)/build --libdir=$(pwd)/build/lib --enable-cassert --enable-debug --without-icu --without-readline --without-zlib
$ make -j8
$ make install
$ export PATH="$PATH:$(pwd)/build/bin"
```

Then grab and build this repo:

```console
$ cd ~
$ git clone https://github.com/eatonphil/pgweb
$ cd pgweb
$ make && make install
```

Create a new Postgres database and run the test script to define a web
service and start the server:

```
$ initdb testdata
$ pg_ctl -D testdata -l logfile start
waiting for server to start.... done
server started
$ cat test.sql
DROP EXTENSION IF EXISTS pgweb;
CREATE EXTENSION pgweb;

DROP FUNCTION IF EXISTS handle_hello_world;
CREATE FUNCTION handle_hello_world(params JSON) RETURNS TEXT AS $$
BEGIN
  RETURN 'Hello, ' || (params->'name') || '!';
END;
$$ LANGUAGE plpgsql;

SELECT pgweb.register_get('/hello', 'handle_hello_world');

SELECT pgweb.serve('localhost', 9003);
$ psql postgres -f test.sql
psql:test.sql:1: NOTICE:  extension "pgweb" does not exist, skipping
DROP EXTENSION
CREATE EXTENSION
psql:test.sql:4: NOTICE:  function handle_hello_world() does not exist, skipping
DROP FUNCTION
CREATE FUNCTION
 register_get
--------------

(1 row)

psql:test.sql:13: INFO:  Listening on localhost:9003.
```

Now in another terminal you can curl the web server.

```console
$ curl 'localhost:9003/hello?name=James'
Hello, James!
```
