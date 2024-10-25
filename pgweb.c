#include <sys/types.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include "postgres.h"
#include "nodes/pg_list.h"
#include "catalog/pg_type_d.h"
#include "utils/builtins.h"
#include "utils/json.h"
#include "utils/memutils.h"
#include "utils/regproc.h"
#include "fmgr.h"

PG_MODULE_MAGIC;

typedef struct PGWHandler {
  char *route;
  char *funcname;
} PGWHandler;

static MemoryContext PGWServerContext;
static List /* PGWHandler * */ *handlers;

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
  MemoryContext context;
  PGWRequestMethod method;
  char *path;
  List /* PGWRequestParam * */ *params;
} PGWRequest;

static PGWRequestMethod
pgweb_parse_request_method(char *buf, int buflen, int *bufp, char **errmsg)
{
  int bufp_original = *bufp;
  int len;
  while (*bufp < buflen && buf[*bufp] != ' ')
	(*bufp)++;

  len = *bufp - bufp_original - 1;
  if (len == 3 && strncmp(buf + bufp_original, "GET", len))
	return PGW_REQUEST_METHOD_GET;

  if (len == 4 && strncmp(buf + bufp_original, "POST", len))
	return PGW_REQUEST_METHOD_POST;

  *errmsg = psprintf("Unsupported method: '%s'", pnstrdup(buf + bufp_original, len));
  return -1;
}

static void
pgweb_parse_request_url(char *buf, int buflen, int *bufp, PGWRequest *request, char **errmsg)
{
  int bufp_original = *bufp;
  int len;
  char *key;
  char *value;
  PGWRequestParam *param;
  bool path_found;

  request->params = NIL;
  while (*bufp < buflen && buf[*bufp] != ' ')
  {
	len = *bufp - bufp_original - 1;
	if (buf[*bufp] == '?')
	{
	  request->path = pnstrdup(buf + bufp_original, len);
	  path_found = true;
	  (*bufp)++;
	  bufp_original = *bufp;
	  continue;
	}

	if (buf[*bufp] == '=')
	{
	  key = pnstrdup(buf + bufp_original, len);
	  (*bufp)++;
	  bufp_original = *bufp;
	  continue;
	}

	if (buf[*bufp] == '&')
	{
	  value = pnstrdup(buf + bufp_original, len);
	  (*bufp)++;
	  bufp_original = *bufp;

	  param = palloc0(sizeof(PGWRequestParam));
	  param->key = key;
	  param->value = value;
	  request->params = lappend(request->params, param);
	  continue;
	}

	(*bufp)++;
  }

  len = *bufp - bufp_original - 1;
  if (!path_found)
	request->path = pnstrdup(buf + bufp_original, len);	
  else if (strlen(key) > 0)
  {
	param = palloc0(sizeof(PGWRequestParam));
	param->key = key;
	param->value = value;
	request->params = lappend(request->params, param);
  }
}

static PGWRequest*
pgweb_parse_request(char *buf, int buflen, char **errmsg)
{
  int bufp = 0;
  PGWRequest *request = palloc0(sizeof(PGWRequest));
  request->context = CurrentMemoryContext;

  request->method = pgweb_parse_request_method(buf, buflen, &bufp, errmsg);
  if (request->method == -1)
	return NULL;

  Assert(buf[bufp] == ' ');
  bufp++;

  pgweb_parse_request_url(buf, buflen, &bufp, request, errmsg);
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
							 CStringGetTextDatum(json_string.data));
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
	elog(ERROR, "Failed to send response to client.");
}

static void
pgweb_handle_request(PGWRequest *request, PGWHandler *handler, char **errmsg)
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
  
  Assert(CurrentMemoryContext == request->context);

  fmgr_info(func_oid, &func_info);
  result = FunctionCall1(&func_info, params);

  pgweb_send_response(request->conn_fd, 200, "OK", TextDatumGetCString(result));
}

static void
pgweb_handle_connection(int client_fd)
{
  char *buf;
  ssize_t n;
  MemoryContext PGWConnectionContext = AllocSetContextCreate(PGWServerContext,
															 "PGWConnectionContext",
															 ALLOCSET_DEFAULT_SIZES);
  char *errmsg = NULL;
  ListCell *lc;
  PGWRequest *request = NULL;
  bool handler_found = false;
  int errcode = 500;

  buf = palloc(4096);
  n = recv(client_fd, buf, 4096, 0);

  // Let's just not support longer requests.
  if (n == 4096)
  {
	errmsg = "Request is too long.";
	goto done;
  }

  request = pgweb_parse_request(buf, buflen, &errmsg);
  if (errmsg != NULL)
	goto done;

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
	pgweb_sendresponse(client_fd,
					   errcode,
					   errcode == 404 ? "Not Found" : "Internal Server Error",
					   errmsg);

  MemoryContextReset(PGWConnectionContext);
}

PG_FUNCTION_INFO_V1(pgweb_register_get);
Datum
pgweb_register_get(PG_FUNCTION_ARGS)
{
  int32 arg = PG_GETARG_INT32(0);
  elog(ERROR, "unimplemented");
  PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(pgweb_serve);
Datum
pgweb_serve(PG_FUNCTION_ARGS)
{
  char *address;
  int32 port = PG_GETARG_INT32(1);
  int server_fd;
  struct sockaddr_un my_addr;

  PGWServerContext = AllocSetContextCreate(TopMemoryContext,
										   "PGWServerContext",
										   ALLOCSET_DEFAULT_SIZES);

  MemoryContextSwitchTo(PGWServerContext);
  address = text_to_cstring(PG_GETARG_TEXT_PP(0));

  memset(&my_addr, 0, sizeof(my_addr));
  my_addr.sin_family = AF_INET;
  my_addr.sin_addr.s_addr = inet_addr(address);
  my_addr.sin_port = htons(port);

  server_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (server_fd == -1)
	elog(ERROR, "Could not create socket.");

  if (bind(server_fd, (struct sockaddr *) &my_addr, sizeof(my_addr)) == -1)
	elog(ERROR, "Could not bind to %s:%d.", address, port);

  if (listen(server_fd, 10 /* Listen backlog. */) == -1)
	elog(ERROR, "Could not listen to %s:%d.", address, port);

  while (1)
  {
	struct sockaddr_un peer_addr;
	socklen_t peer_addr_size;
	int client_fd = accept(server_fd, (struct sockaddr *) &peer_addr, &peer_addr_size);
	if (client_fd == -1)
	  elog(ERROR, "Could not accept connection.");

	pgweb_handle_connection(client_fd);
  }

  PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(pgweb_shutdown);
Datum pgweb_shutdown(PG_FUNCTION_ARGS)
{
  MemoryContextReset(PGWServerContext);
  PG_RETURN_VOID();
}
