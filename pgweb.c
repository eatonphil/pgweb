#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>

#include "postgres.h"

#include "catalog/pg_type_d.h"
#include "fmgr.h"
#include "nodes/pg_list.h"
#include "parser/parse_func.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/json.h"
#include "utils/memutils.h"
#include "utils/regproc.h"

PG_MODULE_MAGIC;

typedef struct PGWHandler {
  char *route;
  char *funcname;
} PGWHandler;

typedef struct PGWResponseCache {
  char *url;
  char *response;
} PGWResponseCache;

static MemoryContext PGWServerContext;
static MemoryContext PGWConnectionContext;
static List /* PGWHandler * */ *handlers;
static List /* PGWResponseCache * */ *response_cache;

typedef enum PGWRequestMethod {
  PGW_REQUEST_METHOD_GET,
  PGW_REQUEST_METHOD_POST,
} PGWRequestMethod ;

typedef struct PGWRequestParam {
  char *key;
  char *value;
} PGWRequestParam;

typedef struct PGWRequest {
  int conn_fd;
  char *buf;
  MemoryContext context;
  PGWRequestMethod method;
  char *url;
  char *path;
  List /* PGWRequestParam * */ *params;
} PGWRequest;

static PGWRequestMethod
pgweb_parse_request_method(PGWRequest *r, int buflen, int *bufp, char **errmsg)
{
  int bufp_original = *bufp;
  int len;

  Assert(CurrentMemoryContext == r->context);

  while (*bufp < buflen && r->buf[*bufp] != ' ')
	(*bufp)++;

  if (*bufp == buflen)
  {
	*errmsg = psprintf("Incomplete request: '%s'", pnstrdup(r->buf, buflen));
	return -1;
  }

  len = *bufp - bufp_original;
  if (len == 3 && strncmp(r->buf + bufp_original, "GET", len) == 0)
	return PGW_REQUEST_METHOD_GET;

  if (len == 4 && strncmp(r->buf + bufp_original, "POST", len) == 0)
	return PGW_REQUEST_METHOD_POST;

  *errmsg = psprintf("Unsupported method: '%s'", pnstrdup(r->buf + bufp_original, len));
  return -1;
}

static void
pgweb_parse_request_url(PGWRequest *r, int buflen, int *bufp, char **errmsg)
{
  int bufp_original = *bufp;
  int bufp_tmp = *bufp;
  int len = 0;
  char *key = NULL;
  char *value = NULL;
  PGWRequestParam *param = NULL;
  bool path_found = false;

  Assert(CurrentMemoryContext == r->context);

  r->params = NIL;
  while (*bufp < buflen && r->buf[*bufp] != ' ')
  {
	len = *bufp - bufp_tmp;
	if (r->buf[*bufp] == '?')
	{
	  r->path = pnstrdup(r->buf + bufp_tmp, len);
	  path_found = true;
	  (*bufp)++;
	  bufp_tmp = *bufp;
	  continue;
	}

	if (r->buf[*bufp] == '=')
	{
	  key = pnstrdup(r->buf + bufp_tmp, len);
	  (*bufp)++;
	  bufp_tmp = *bufp;
	  continue;
	}

	if (r->buf[*bufp] == '&')
	{
	  value = pnstrdup(r->buf + bufp_tmp, len);
	  (*bufp)++;
	  bufp_tmp = *bufp;

	  param = palloc0(sizeof(PGWRequestParam));
	  param->key = key;
	  param->value = value;
	  r->params = lappend(r->params, param);
	  continue;
	}

	(*bufp)++;
  }

  len = *bufp - bufp_original;
  if (!path_found)
	r->path = pnstrdup(r->buf + bufp_original, len);
  else if (key != NULL && strlen(key) > 0)
  {
	param = palloc0(sizeof(PGWRequestParam));
	param->key = key;
	param->value = pnstrdup(r->buf + bufp_tmp, *bufp - bufp_tmp);
	*bufp += len;
	r->params = lappend(r->params, param);
  }

  r->url = pnstrdup(r->buf + bufp_original, len);
}

static PGWRequest*
pgweb_parse_request(MemoryContext requestContext, char *buf, int buflen, char **errmsg)
{
  int bufp = 0;
  PGWRequest *request = palloc0(sizeof(PGWRequest));

  request->buf = buf;
  request->context = requestContext;
  Assert(CurrentMemoryContext == request->context);

  request->method = pgweb_parse_request_method(request, buflen, &bufp, errmsg);
  if (request->method == -1)
	return NULL;

  Assert(request->buf[bufp] == ' ');
  bufp++;

  pgweb_parse_request_url(request, buflen, &bufp, errmsg);
  return request;
}

static Datum
pgweb_request_params_to_json(PGWRequest *request)
{
  ListCell *lc;
  StringInfoData json_string;

  initStringInfo(&json_string);
  appendStringInfoString(&json_string, "{");

  foreach (lc, request->params)
  {
	PGWRequestParam *param = lfirst(lc);
	if (json_string.len > 1)
	  appendStringInfoString(&json_string, ", ");

	/* We're just going to assume there's no quotes in key or value. */
	appendStringInfo(&json_string,
					 "\"%s\": \"%s\"",
					 param->key,
					 param->value);
  }

  appendStringInfoString(&json_string, "}");

  return DirectFunctionCall1(json_in,
							 CStringGetDatum(json_string.data));
}

static void
pgweb_send_response(int conn_fd, int code, char *status, char *body)
{
  char *buf = psprintf("HTTP/1.1 %d %s\r\n"
					   "Content-Length: %lu\r\n"
					   "Content-Type: text/plain\r\n"
					   "\r\n"
					   "%s",
					   code,
					   status,
					   strlen(body),
					   body);
  ssize_t n = send(conn_fd, buf, strlen(buf), 0);
  if (n != strlen(buf))
  {
	int e = errno;
	elog(ERROR, "Failed to send response to client: %s.", strerror(e));
  }
}

static void
pgweb_handle_request(PGWRequest *request, PGWHandler *handler, char **errmsg)
{
  ListCell *lc;
  char *msg = NULL;
  PGWResponseCache *cached = NULL;

  // If there's a cached response, use it.
  foreach (lc, response_cache)
  {
	cached = lfirst(lc);
	if (strcmp(cached->url, request->url) == 0)
	{
	  msg = cached->response;
	  elog(INFO, "Cached request.");
	  break;
	}
  }

  if (msg == NULL)
  {
	List *func_name_list = stringToQualifiedNameList(handler->funcname, NULL);
	Oid argtypes[] = {JSONOID};
	Oid func_oid = LookupFuncName(func_name_list,
								  sizeof(argtypes) / sizeof(Oid),
								  argtypes,
								  false);
	FmgrInfo func_info;
	Datum params = pgweb_request_params_to_json(request);
	Datum result;

	fmgr_info(func_oid, &func_info);
	result = FunctionCall1(&func_info, params);
	msg = TextDatumGetCString(result);

	/* Cache this response for the future. */
	Assert(CurrentMemoryContext == request->context);
	MemoryContextSwitchTo(PGWServerContext);
	cached = palloc0(sizeof(*cached));
	cached->url = pstrdup(request->url);
	cached->response = pstrdup(msg);
	response_cache = lappend(response_cache, cached);
	MemoryContextSwitchTo(request->context);
  }

  pgweb_send_response(request->conn_fd, 200, "OK", msg);
}

static bool
pgweb_handle_connection(int client_fd)
{
  char *buf;
  ssize_t n;
  char *errmsg = NULL;
  ListCell *lc;
  PGWRequest *request = NULL;
  bool handler_found = false;
  int errcode = 500;
  MemoryContext oldctx;
  clock_t start = clock();
  clock_t stop;
  bool stayalive = true;

  if (PGWConnectionContext == NULL)
	PGWConnectionContext = AllocSetContextCreate(PGWServerContext,
												 "PGWConnectionContext",
												 ALLOCSET_DEFAULT_SIZES);

  oldctx = MemoryContextSwitchTo(PGWConnectionContext);

  buf = palloc(4096);
  n = recv(client_fd, buf, 4096, 0);

  // Let's just not support longer requests.
  if (n == 4096)
  {
	errmsg = "Request is too long.";
	goto done;
  }

  request = pgweb_parse_request(PGWConnectionContext, buf, n, &errmsg);
  if (errmsg != NULL)
	goto done;

  request->conn_fd = client_fd;
  if (strcmp(request->url, "/_exit") == 0)
  {
  	stayalive = false;
	goto done;
  }

  foreach (lc, handlers)
  {
	PGWHandler *handler = lfirst(lc);
	if (strcmp(handler->route, request->path) == 0)
	{
	  pgweb_handle_request(request, handler, &errmsg);
	  handler_found = true;
	  break;
	}
  }

  if (!handler_found)
  {
	errcode = 404;
	errmsg = "Not found";
  }

 done:
  if (errmsg)
	pgweb_send_response(client_fd,
					   errcode,
					   errcode == 404 ? "Not Found" : "Internal Server Error",
					   errmsg);

  stop = clock();
  elog(INFO, "[%fs] %s %s",
	   (double)(stop - start) / CLOCKS_PER_SEC,
	   request->method == PGW_REQUEST_METHOD_GET ? "GET" : "POST",
	   request->url);

  Assert(CurrentMemoryContext == PGWConnectionContext);
  MemoryContextReset(PGWConnectionContext);
  MemoryContextSwitchTo(oldctx);

  return stayalive;
}

PG_FUNCTION_INFO_V1(pgweb_register_get);
Datum
pgweb_register_get(PG_FUNCTION_ARGS)
{
  MemoryContext oldctx;
  PGWHandler *handler;

  oldctx = MemoryContextSwitchTo(PGWServerContext);

  handler = palloc(sizeof(PGWHandler));
  handler->route = TextDatumGetCString(PG_GETARG_DATUM(0));
  handler->funcname = TextDatumGetCString(PG_GETARG_DATUM(1));
  handlers = lappend(handlers, handler);

  MemoryContextSwitchTo(oldctx);

  PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(pgweb_serve);
Datum
pgweb_serve(PG_FUNCTION_ARGS)
{
  char *address;
  int32 port = PG_GETARG_INT32(1);
  int server_fd;
  struct sockaddr_in server_addr;

  MemoryContextSwitchTo(PGWServerContext);
  address = TextDatumGetCString(PG_GETARG_DATUM(0));

  memset(&server_addr, 0, sizeof(server_addr));
  server_addr.sin_family = AF_INET;
  server_addr.sin_addr.s_addr = htonl(INADDR_ANY); //inet_addr(address);
  server_addr.sin_port = htons(port);

  server_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (server_fd == -1)
  {
	int e = errno;
	elog(ERROR, "Could not create socket: %s.", strerror(e));
  }

  if (bind(server_fd, (struct sockaddr *) &server_addr, sizeof(server_addr)) == -1)
  {
	int e = errno;
	elog(ERROR, "Could not bind to %s:%d: %s.", address, port, strerror(e));
  }

  if (listen(server_fd, 10 /* Listen backlog. */) == -1)
  {
	int e = errno;
	elog(ERROR, "Could not listen to %s:%d: %s.", address, port, strerror(e));
  }

  elog(INFO, "Listening on %s:%d.", address, port);

  while (1)
  {
	struct sockaddr_in peer_addr;
	socklen_t peer_addr_size;
	int client_fd = accept(server_fd, (struct sockaddr *) &peer_addr, &peer_addr_size);
	if (client_fd == -1)
	{
	  int e = errno;
	  elog(ERROR, "Could not accept connection: %s.", strerror(e));
	}

	if (!pgweb_handle_connection(client_fd))
	{
	  elog(INFO, "Shutting down.");
	  break;
	}
  }

  PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(pgweb_shutdown);
Datum pgweb_shutdown(PG_FUNCTION_ARGS)
{
  MemoryContextReset(PGWServerContext);
  PG_RETURN_VOID();
}

void _PG_init(void)
{

  PGWServerContext = AllocSetContextCreate(TopMemoryContext,
										   "PGWServerContext",
										   ALLOCSET_DEFAULT_SIZES);
}
