/* CGSGL v2. Request-owned PHP 8.2 NTS binding; native ABI owns validation. */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include <php.h>
#include <Zend/zend_exceptions.h>
#include <ext/standard/info.h>
#include <unistd.h>
#include "datarouter_frontend.h"
#include "datarouter_protocol.h"
#include "version.h"
#if PHP_VERSION_ID < 80200 || PHP_VERSION_ID >= 80300 || defined(ZTS)
#error "DataRouter requires PHP 8.2 NTS"
#endif
static int dr_resource;
static zend_class_entry *dr_exception;
typedef struct { datarouter_frontend *port; pid_t owner; } dr_handle;
static void fail(int code) {
    zend_throw_exception_ex(dr_exception, code, "DataRouter error=%d", code);
}
static void destroy(zend_resource *r) {
    dr_handle *h = r->ptr;
    if (h->port && h->owner == getpid()) datarouter_frontend_close(h->port);
    efree(h);
}
static dr_handle *handle(zval *r, zend_bool allow_closed) {
    dr_handle *h = zend_fetch_resource_ex(r, "DataRouter", dr_resource);
    if (!h) return NULL;
    if (h->owner != getpid() || (!allow_closed && !h->port)) {
        fail(DATAROUTER_ERR_INVALID_ARGUMENT); return NULL;
    }
    return h;
}
static int timeout_ok(zend_long n) {
    if (n < 0 || (zend_ulong)n > UINT32_MAX) { fail(DATAROUTER_ERR_INVALID_ARGUMENT); return 0; }
    return 1;
}
PHP_FUNCTION(datarouter_open) {
    char *shm, *service, *token = ""; size_t sn, nn, tn = 0;
    zend_long plane = 0, timeout = 1000;
    ZEND_PARSE_PARAMETERS_START(2, 5)
        Z_PARAM_PATH(shm, sn) Z_PARAM_PATH(service, nn)
        Z_PARAM_OPTIONAL Z_PARAM_LONG(plane) Z_PARAM_LONG(timeout) Z_PARAM_PATH(token, tn)
    ZEND_PARSE_PARAMETERS_END();
    if (!timeout_ok(timeout)) RETURN_THROWS();
    if (plane != 0 && plane != 1) { fail(DATAROUTER_ERR_INVALID_ARGUMENT); RETURN_THROWS(); }
    datarouter_frontend_options_v3 o = {0};
    o.shm_name = shm; o.service_name = service; o.auth_token = token;
    o.data_plane = (uint8_t)plane; o.connect_timeout_ms = timeout; o.recv_timeout_ms = timeout;
    datarouter_frontend *p = NULL;
    int rc = datarouter_frontend_open_v3(&o, sizeof(o), DATAROUTER_FRONTEND_ABI_VERSION_V3, &p);
    if (rc) { fail(rc); RETURN_THROWS(); }
    dr_handle *h = emalloc(sizeof(*h)); h->port = p; h->owner = getpid();
    RETURN_RES(zend_register_resource(h, dr_resource));
}
PHP_FUNCTION(datarouter_close) {
    zval *r; ZEND_PARSE_PARAMETERS_START(1, 1) Z_PARAM_RESOURCE(r) ZEND_PARSE_PARAMETERS_END();
    dr_handle *h = handle(r, 1); if (!h) RETURN_THROWS();
    if (h->port) { datarouter_frontend_close(h->port); h->port = NULL; }
    RETURN_NULL();
}
PHP_FUNCTION(datarouter_send) {
    zval *r; char *target, *data; size_t tn, n; zend_long timeout = 1000;
    ZEND_PARSE_PARAMETERS_START(3, 4)
        Z_PARAM_RESOURCE(r) Z_PARAM_PATH(target, tn) Z_PARAM_STRING(data, n)
        Z_PARAM_OPTIONAL Z_PARAM_LONG(timeout)
    ZEND_PARSE_PARAMETERS_END();
    dr_handle *h = handle(r, 0); if (!h || !timeout_ok(timeout)) RETURN_THROWS();
    int rc = datarouter_frontend_send(h->port, target, data, n, timeout);
    if (rc) { fail(rc); RETURN_THROWS(); } RETURN_NULL();
}
PHP_FUNCTION(datarouter_recv) {
    zval *r; zend_long timeout = 1000;
    ZEND_PARSE_PARAMETERS_START(1, 2) Z_PARAM_RESOURCE(r) Z_PARAM_OPTIONAL Z_PARAM_LONG(timeout) ZEND_PARSE_PARAMETERS_END();
    dr_handle *h = handle(r, 0); if (!h || !timeout_ok(timeout)) RETURN_THROWS();
    datarouter_message_info info = {0};
    size_t cap = 4096;
    zend_string *s = zend_string_alloc(cap, 0);
    int rc = datarouter_frontend_recv(h->port, ZSTR_VAL(s), cap, &info, timeout);
    if (rc == DATAROUTER_ERR_PAYLOAD_TOO_LARGE) {
        cap = info.len;
        zend_string_release(s); s = zend_string_alloc(cap, 0);
        rc = datarouter_frontend_recv(h->port, ZSTR_VAL(s), cap, &info, timeout);
    }
    if (rc) { zend_string_release(s); fail(rc); RETURN_THROWS(); }
    ZSTR_LEN(s) = info.len; ZSTR_VAL(s)[info.len] = 0;
    array_init(return_value); add_assoc_str(return_value, "data", s);
    add_assoc_string(return_value, "source", info.source);
}
PHP_FUNCTION(datarouter_pack) {
    char *body, *vars = ""; size_t n, vn = 0; zend_long type = 7;
    ZEND_PARSE_PARAMETERS_START(1, 3) Z_PARAM_STRING(body, n)
        Z_PARAM_OPTIONAL Z_PARAM_LONG(type) Z_PARAM_STRING(vars, vn)
    ZEND_PARSE_PARAMETERS_END();
    if (type < 0 || type > 255) { fail(DATAROUTER_ERR_INVALID_ARGUMENT); RETURN_THROWS(); }
    datarouter_message *m = datarouter_msg_new(type);
    if (!m) { fail(DATAROUTER_ERR_INTERNAL); RETURN_THROWS(); }
    datarouter_msg_set_body(m, body, n); datarouter_msg_set_var_data(m, vars, vn);
    size_t size = 0, written = 0;
    int probe = datarouter_msg_pack(m, NULL, 0, &size);
    if (probe != DATAROUTER_ERR_PAYLOAD_TOO_LARGE && probe != DATAROUTER_OK) {
        datarouter_msg_free(m); fail(probe); RETURN_THROWS();
    }
    zend_string *s = zend_string_alloc(size, 0);
    int rc = datarouter_msg_pack(m, ZSTR_VAL(s), size, &written);
    datarouter_msg_free(m);
    if (rc) { zend_string_release(s); fail(rc); RETURN_THROWS(); }
    ZSTR_LEN(s) = written; ZSTR_VAL(s)[written] = 0; RETURN_STR(s);
}
PHP_FUNCTION(datarouter_unpack) {
    char *wire; size_t n; ZEND_PARSE_PARAMETERS_START(1, 1) Z_PARAM_STRING(wire, n) ZEND_PARSE_PARAMETERS_END();
    datarouter_message *m = NULL; int rc = datarouter_msg_unpack(wire, n, &m);
    if (rc) { fail(rc); RETURN_THROWS(); }
    array_init(return_value); add_assoc_long(return_value, "type", datarouter_msg_type(m));
    add_assoc_long(return_value, "flags", datarouter_msg_flags(m));
    const char *body = (const char *)datarouter_msg_body(m);
    const char *vars = (const char *)datarouter_msg_var_data(m);
    add_assoc_stringl(return_value, "body", body ? body : "", datarouter_msg_body_len(m));
    add_assoc_stringl(return_value, "var_data", vars ? vars : "", datarouter_msg_var_data_len(m));
    datarouter_msg_free(m);
}
PHP_FUNCTION(datarouter_max_payload) { ZEND_PARSE_PARAMETERS_NONE(); RETURN_LONG(datarouter_max_app_payload()); }
ZEND_BEGIN_ARG_INFO_EX(a_open, 0, 0, 2)
    ZEND_ARG_TYPE_INFO(0, shm, IS_STRING, 0)
    ZEND_ARG_TYPE_INFO(0, service, IS_STRING, 0)
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, plane, IS_LONG, 0, "0")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, timeout, IS_LONG, 0, "1000")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, token, IS_STRING, 0, "\"\"")
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_INFO_EX(a_close, 0, 0, 1)
    ZEND_ARG_INFO(0, port)
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_INFO_EX(a_send, 0, 0, 3)
    ZEND_ARG_INFO(0, port)
    ZEND_ARG_TYPE_INFO(0, target, IS_STRING, 0)
    ZEND_ARG_TYPE_INFO(0, data, IS_STRING, 0)
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, timeout, IS_LONG, 0, "1000")
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_INFO_EX(a_recv, 0, 0, 1)
    ZEND_ARG_INFO(0, port)
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, timeout, IS_LONG, 0, "1000")
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_INFO_EX(a_pack, 0, 0, 1)
    ZEND_ARG_TYPE_INFO(0, body, IS_STRING, 0)
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, type, IS_LONG, 0, "7")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, var_data, IS_STRING, 0, "\"\"")
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_INFO_EX(a_unpack, 0, 0, 1)
    ZEND_ARG_TYPE_INFO(0, wire, IS_STRING, 0)
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_INFO_EX(a_none, 0, 0, 0)
ZEND_END_ARG_INFO()
static const zend_function_entry functions[] = {
    PHP_FE(datarouter_open, a_open) PHP_FE(datarouter_close, a_close)
    PHP_FE(datarouter_send, a_send) PHP_FE(datarouter_recv, a_recv)
    PHP_FE(datarouter_pack, a_pack) PHP_FE(datarouter_unpack, a_unpack)
    PHP_FE(datarouter_max_payload, a_none) PHP_FE_END
};
PHP_MINIT_FUNCTION(datarouter) {
    dr_resource = zend_register_list_destructors_ex(destroy, NULL, "DataRouter", module_number);
    zend_class_entry ce; INIT_CLASS_ENTRY(ce, "DataRouterException", NULL);
    dr_exception = zend_register_internal_class_ex(&ce, zend_ce_exception);
    REGISTER_LONG_CONSTANT("DATAROUTER_DIRECT", 0, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("DATAROUTER_ROUTED", 1, CONST_CS | CONST_PERSISTENT);
    REGISTER_STRING_CONSTANT("DATAROUTER_GIT_COMMIT", CGEOS_GIT_COMMIT, CONST_CS | CONST_PERSISTENT);
    return SUCCESS;
}
PHP_MINFO_FUNCTION(datarouter) {
    php_info_print_table_start();
    php_info_print_table_row(2, "DataRouter version", CGEOS_GIT_COMMIT);
    php_info_print_table_end();
}
zend_module_entry datarouter_module_entry = {
    STANDARD_MODULE_HEADER, "datarouter", functions, PHP_MINIT(datarouter), NULL,
    NULL, NULL, PHP_MINFO(datarouter), CGEOS_GIT_COMMIT, STANDARD_MODULE_PROPERTIES
};
#ifdef COMPILE_DL_DATAROUTER
ZEND_GET_MODULE(datarouter)
#endif
