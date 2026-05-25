// KoBun is a simple JS runtime written in C using JavaScriptCore.
// Copyright (c) 2026 Yuchi Yamaguchi under the MIT License.

#include <JavaScriptCore/JavaScriptCore.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

// Atomic operation for event loop.
typedef struct Task {
    // unique task id
    int id;

    // deadline in milliseconds in Unix time. For Promise.then, it can be 0 or
    // ignored.
    int64_t deadline_ms;

    // callback function. Firstly we support no-argument callback for
    // simplicity.
    JSObjectRef callback;

    // next task in the queue
    struct Task *next;
} Task;

// Classes that we define in C and expose to JavaScript
typedef struct HostClasses {
    JSClassRef response;
} HostClasses;

typedef struct ServerResource {
    int listen_fd;
    int port;
    char *hostname;
    JSObjectRef fetch;
    bool active;
} ServerResource;

typedef struct Runtime {
    // JavaScriptCore context
    JSGlobalContextRef ctx;

    // task queue
    Task *task_queue_head;
    Task *task_queue_tail;

    // counter for generating unique task id
    int _task_id_counter;

    // host classes
    HostClasses host_classes;

    // server resource
    ServerResource server;
} Runtime;

int gen_task_id(Runtime *rt) {
    rt->_task_id_counter++;
    return rt->_task_id_counter;
}

void init_server(ServerResource *server) {
    server->listen_fd = -1;
    server->port = 0;
    server->hostname = NULL;
    server->fetch = NULL;
    server->active = false;
}

void destroy_server(Runtime *rt, ServerResource *server) {
    if (server->fetch) {
        JSValueUnprotect(rt->ctx, server->fetch);
    }

    if (server->listen_fd >= 0) {
        close(server->listen_fd);
    }

    free(server->hostname);
    init_server(server);
}

// ==== Utilities ====

char *read_file(char *path) {
    FILE *file = fopen(path, "r");
    if (!file) {
        return NULL;
    }

    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    rewind(file);

    char *content = malloc(size + 1);
    fread(content, 1, size, file);
    content[size] = '\0';

    fclose(file);
    return content;
}

char *jsvalue_to_cstring(JSContextRef ctx, JSValueRef value) {
    JSStringRef str = JSValueToStringCopy(ctx, value, NULL);
    if (str == NULL) return NULL;

    size_t size = JSStringGetMaximumUTF8CStringSize(str);
    char *cstr = malloc(size);
    if (cstr != NULL) {
        JSStringGetUTF8CString(str, cstr, size);
    }

    JSStringRelease(str);
    return cstr;
}

void print_jsvalue(JSContextRef ctx, JSValueRef value) {
    char *cstr = jsvalue_to_cstring(ctx, value);
    if (cstr == NULL) return;

    printf("%s", cstr);
    free(cstr);
}

JSValueRef make_js_string(JSContextRef ctx, const char *value) {
    JSStringRef str = JSStringCreateWithUTF8CString(value);
    JSValueRef js_value = JSValueMakeString(ctx, str);
    JSStringRelease(str);
    return js_value;
}

JSValueRef get_property(JSContextRef ctx, JSObjectRef object,
                        const char *name) {
    JSStringRef key = JSStringCreateWithUTF8CString(name);
    JSValueRef value = JSObjectGetProperty(ctx, object, key, NULL);
    JSStringRelease(key);
    return value;
}

void set_property(JSContextRef ctx, JSObjectRef object, const char *name,
                  JSValueRef value) {
    JSStringRef key = JSStringCreateWithUTF8CString(name);
    JSObjectSetProperty(ctx, object, key, value, kJSPropertyAttributeNone,
                        NULL);
    JSStringRelease(key);
}

char *copy_span(const char *start, size_t len) {
    char *copy = malloc(len + 1);
    if (copy == NULL) return NULL;

    memcpy(copy, start, len);
    copy[len] = '\0';
    return copy;
}

bool is_ows(char c) { return c == ' ' || c == '\t'; }

char *copy_trimmed_span(const char *start, size_t len) {
    while (len > 0 && is_ows(*start)) {
        start++;
        len--;
    }

    while (len > 0 && is_ows(start[len - 1])) {
        len--;
    }

    return copy_span(start, len);
}

void lowercase_ascii(char *text) {
    for (char *p = text; *p; p++) {
        if (*p >= 'A' && *p <= 'Z') {
            *p = *p - 'A' + 'a';
        }
    }
}

const char *find_line_end(const char *start) {
    const char *crlf = strstr(start, "\r\n");
    const char *lf = strchr(start, '\n');

    if (crlf && lf) return crlf < lf ? crlf : lf;
    if (crlf) return crlf;
    if (lf) return lf;
    return start + strlen(start);
}

const char *next_line_start(const char *line_end) {
    if (line_end[0] == '\r' && line_end[1] == '\n') {
        return line_end + 2;
    }
    if (line_end[0] == '\n') {
        return line_end + 1;
    }
    return line_end;
}

void println_jsvalue(JSContextRef ctx, JSValueRef value) {
    print_jsvalue(ctx, value);
    printf("\n");
}

int64_t current_time_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void sleep_until(int64_t deadline_ms) {
    int64_t now = current_time_ms();
    if (deadline_ms > now) {
        struct timespec sleep_time;
        sleep_time.tv_sec = (deadline_ms - now) / 1000;
        sleep_time.tv_nsec = ((deadline_ms - now) % 1000) * 1000000;
        nanosleep(&sleep_time, NULL);
    }
}

// ==== HTTP request parsing ====

typedef struct ParsedRequest {
    char *method;
    char *url;
    char *path;
    char *query;
} ParsedRequest;

void init_parsed_request(ParsedRequest *request) {
    request->method = NULL;
    request->url = NULL;
    request->path = NULL;
    request->query = NULL;
}

void destroy_parsed_request(ParsedRequest *request) {
    free(request->method);
    free(request->url);
    free(request->path);
    free(request->query);
    init_parsed_request(request);
}

bool parse_request_line(const char *raw, ParsedRequest *request) {
    if (raw == NULL || request == NULL) return false;

    init_parsed_request(request);

    const char *line_end = find_line_end(raw);
    const char *method_end = memchr(raw, ' ', (size_t)(line_end - raw));
    if (method_end == NULL || method_end == raw) return false;

    const char *url_start = method_end + 1;
    const char *url_end =
        memchr(url_start, ' ', (size_t)(line_end - url_start));
    if (url_end == NULL || url_end == url_start) return false;

    const char *version_start = url_end + 1;
    if (version_start >= line_end) return false;

    request->method = copy_span(raw, (size_t)(method_end - raw));
    request->url = copy_span(url_start, (size_t)(url_end - url_start));

    const char *query_start =
        memchr(url_start, '?', (size_t)(url_end - url_start));
    if (query_start) {
        request->path = copy_span(url_start, (size_t)(query_start - url_start));
        request->query =
            copy_span(query_start + 1, (size_t)(url_end - query_start - 1));
    } else {
        request->path = copy_span(url_start, (size_t)(url_end - url_start));
        request->query = copy_span("", 0);
    }

    if (request->method == NULL || request->url == NULL ||
        request->path == NULL || request->query == NULL) {
        destroy_parsed_request(request);
        return false;
    }

    return true;
}

JSObjectRef parse_request_headers(JSContextRef ctx, const char *raw) {
    JSObjectRef headers = JSObjectMake(ctx, NULL, NULL);
    const char *line_end = find_line_end(raw);
    const char *cursor = next_line_start(line_end);

    while (*cursor) {
        line_end = find_line_end(cursor);
        size_t line_len = (size_t)(line_end - cursor);
        if (line_len > 0 && cursor[line_len - 1] == '\r') {
            line_len--;
        }

        if (line_len == 0) return headers;

        const char *colon = memchr(cursor, ':', line_len);
        if (colon) {
            char *name = copy_trimmed_span(cursor, (size_t)(colon - cursor));
            char *value = copy_trimmed_span(
                colon + 1, line_len - (size_t)(colon - cursor) - 1);

            if (name && value && name[0] != '\0') {
                lowercase_ascii(name);
                set_property(ctx, headers, name, make_js_string(ctx, value));
            }
            free(name);
            free(value);
        }

        cursor = next_line_start(line_end);
        if (cursor == line_end) return headers;
    }

    return headers;
}

JSObjectRef make_request_object(JSContextRef ctx, ParsedRequest *request,
                                JSObjectRef headers) {
    JSObjectRef req = JSObjectMake(ctx, NULL, NULL);
    set_property(ctx, req, "method", make_js_string(ctx, request->method));
    set_property(ctx, req, "url", make_js_string(ctx, request->url));
    set_property(ctx, req, "path", make_js_string(ctx, request->path));
    set_property(ctx, req, "query", make_js_string(ctx, request->query));
    set_property(ctx, req, "headers", headers);
    return req;
}

// ==== Runtime management ====

void init_rt(Runtime *rt) {
    rt->ctx = NULL;
    rt->host_classes.response = NULL;
    rt->task_queue_head = NULL;
    rt->task_queue_tail = NULL;
    rt->_task_id_counter = 0;
    init_server(&rt->server);
}

void create_context(Runtime *rt) {
    JSClassDefinition kb_class = kJSClassDefinitionEmpty;
    kb_class.className = "KoBunRuntime";
    JSClassRef global_class = JSClassCreate(&kb_class);
    rt->ctx = JSGlobalContextCreate(global_class);
    JSClassRelease(global_class);
}

void attach_runtime(Runtime *rt) {
    JSObjectRef global = JSContextGetGlobalObject(rt->ctx);
    JSObjectSetPrivate(global, rt);
}

Runtime *get_rt(JSContextRef ctx) {
    JSObjectRef global = JSContextGetGlobalObject(ctx);
    return (Runtime *)JSObjectGetPrivate(global);
}

// ==== Server resource ====

bool start_server(Runtime *rt, int port, JSObjectRef fetch) {
    if (rt->server.active) return false;

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) return false;

    int reuse = 1;
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) <
        0) {
        close(listen_fd);
        return false;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(listen_fd);
        return false;
    }

    if (listen(listen_fd, 128) < 0) {
        close(listen_fd);
        return false;
    }

    char *hostname = copy_span("0.0.0.0", strlen("0.0.0.0"));
    if (hostname == NULL) {
        close(listen_fd);
        return false;
    }

    rt->server.listen_fd = listen_fd;
    rt->server.port = port;
    rt->server.hostname = hostname;
    rt->server.fetch = fetch;
    rt->server.active = true;
    JSValueProtect(rt->ctx, fetch);
    return true;
}

JSObjectRef make_server_object(JSContextRef ctx, ServerResource *server) {
    JSObjectRef object = JSObjectMake(ctx, NULL, NULL);
    set_property(ctx, object, "hostname",
                 make_js_string(ctx, server->hostname));
    set_property(ctx, object, "port", JSValueMakeNumber(ctx, server->port));
    return object;
}

// ==== Task queue ====

bool tq_is_empty(Runtime *rt) { return rt->task_queue_head == NULL; }

int64_t tq_next_deadline(Runtime *rt) {
    if (rt->task_queue_head) {
        return rt->task_queue_head->deadline_ms;
    }
    return -1;
}

Task *create_task(Runtime *rt, JSObjectRef callback, int64_t deadline_ms) {
    Task *task = malloc(sizeof(Task));
    task->id = gen_task_id(rt);
    task->callback = callback;
    task->deadline_ms = deadline_ms;
    task->next = NULL;
    JSValueProtect(rt->ctx, callback);
    return task;
}

// Important note: The head of the queue is the task with the earliest deadline.
void tq_push(Runtime *rt, Task *task) {
    if (tq_is_empty(rt)) {
        rt->task_queue_head = task;
        rt->task_queue_tail = task;
        return;
    }

    if (task->deadline_ms < tq_next_deadline(rt)) {
        task->next = rt->task_queue_head;
        rt->task_queue_head = task;
        return;
    }

    Task *prev = rt->task_queue_head;
    while (prev->next && prev->next->deadline_ms <= task->deadline_ms) {
        prev = prev->next;
    }

    task->next = prev->next;
    prev->next = task;

    if (task->next == NULL) {
        rt->task_queue_tail = task;
    }
}

Task *tq_pop(Runtime *rt) {
    Task *task = rt->task_queue_head;
    if (task == NULL) return NULL;

    rt->task_queue_head = task->next;
    if (rt->task_queue_head == NULL) {
        rt->task_queue_tail = NULL;
    }

    task->next = NULL;
    return task;
}

// Insert a task into the task queue. Return the generated task id.
int add_task(Runtime *rt, JSObjectRef callback, int64_t deadline_ms) {
    Task *task = create_task(rt, callback, deadline_ms);
    tq_push(rt, task);
    return task->id;
}

void destroy_task(Runtime *rt, Task *task) {
    JSValueUnprotect(rt->ctx, task->callback);
    free(task);
}

void destroy_pending_tasks(Runtime *rt) {
    while (!tq_is_empty(rt)) {
        Task *task = tq_pop(rt);
        destroy_task(rt, task);
    }
}

bool deadline_overdue(int64_t deadline_ms) {
    return current_time_ms() >= deadline_ms;
}

bool tq_has_due_task(Runtime *rt) {
    return !tq_is_empty(rt) && deadline_overdue(tq_next_deadline(rt));
}

// Execute a task by calling its callback function.
// The callback's return value is ignored, as in setTimeout.
bool execute(Runtime *rt, Task *task) {
    JSValueRef exception = NULL;
    JSObjectCallAsFunction(rt->ctx, task->callback, NULL, 0, NULL, &exception);

    if (exception) {
        printf("Error: ");
        println_jsvalue(rt->ctx, exception);
        return false;
    }

    return true;
}

// Check if there is a task that can be consumed (i.e., its deadline has passed)
// and execute it. Return -1 on failure; otherwise, return a number of executed
// tasks.
int may_consume_task(Runtime *rt) {
    int execute_count = 0;

    while (tq_has_due_task(rt)) {
        Task *task = tq_pop(rt);

        if (!execute(rt, task)) {
            destroy_task(rt, task);
            return -1;
        }

        execute_count++;
        destroy_task(rt, task);
    }

    return execute_count;
}

// ==== Builtin classes/functions ====

JSObjectRef response_constructor(JSContextRef ctx, JSObjectRef constructor,
                                 size_t argumentCount,
                                 const JSValueRef arguments[],
                                 JSValueRef *exception) {
    (void)exception;

    Runtime *rt = get_rt(ctx);
    JSObjectRef response = JSObjectMake(ctx, rt->host_classes.response, NULL);

    JSStringRef prototype_name = JSStringCreateWithUTF8CString("prototype");
    JSValueRef prototype =
        JSObjectGetProperty(ctx, constructor, prototype_name, NULL);
    JSObjectSetPrototype(ctx, response, prototype);
    JSStringRelease(prototype_name);

    JSValueRef body =
        argumentCount > 0 ? arguments[0] : make_js_string(ctx, "");
    set_property(ctx, response, "body", body);

    int status = 200;
    if (argumentCount > 1 && JSValueIsObject(ctx, arguments[1])) {
        JSObjectRef init = JSValueToObject(ctx, arguments[1], NULL);
        JSValueRef status_value = get_property(ctx, init, "status");
        if (JSValueIsNumber(ctx, status_value)) {
            status = (int)JSValueToNumber(ctx, status_value, NULL);
        }
    }
    set_property(ctx, response, "status", JSValueMakeNumber(ctx, status));

    JSObjectRef headers = JSObjectMake(ctx, NULL, NULL);

    if (argumentCount > 1 && JSValueIsObject(ctx, arguments[1])) {
        JSObjectRef init = JSValueToObject(ctx, arguments[1], NULL);
        JSValueRef headers_value = get_property(ctx, init, "headers");

        if (JSValueIsObject(ctx, headers_value)) {
            headers = JSValueToObject(ctx, headers_value, NULL);
        }
    }

    set_property(ctx, response, "headers", headers);

    return response;
}

bool is_response(Runtime *rt, JSContextRef ctx, JSValueRef value) {
    return JSValueIsObjectOfClass(ctx, value, rt->host_classes.response);
}

int read_response_status(JSContextRef ctx, JSObjectRef response) {
    JSValueRef value = get_property(ctx, response, "status");
    if (!JSValueIsNumber(ctx, value)) return 200;
    return (int)JSValueToNumber(ctx, value, NULL);
}

char *read_response_body(JSContextRef ctx, JSObjectRef response) {
    JSValueRef value = get_property(ctx, response, "body");
    return jsvalue_to_cstring(ctx, value);
}

JSObjectRef read_response_headers(JSContextRef ctx, JSObjectRef response) {
    JSValueRef value = get_property(ctx, response, "headers");
    if (!JSValueIsObject(ctx, value)) return NULL;
    return JSValueToObject(ctx, value, NULL);
}

const char *response_reason_phrase(int status) {
    switch (status) {
    case 200:
        return "OK";
    case 400:
        return "Bad Request";
    case 404:
        return "Not Found";
    case 500:
        return "Internal Server Error";
    default:
        return "OK";
    }
}

char *serialize_status_response(int status, const char *body) {
    const char *reason = response_reason_phrase(status);
    size_t body_len = strlen(body);

    int size =
        snprintf(NULL, 0, "HTTP/1.1 %d %s\r\nContent-Length: %zu\r\n\r\n%s",
                 status, reason, body_len, body);
    if (size < 0) return NULL;

    char *serialized = malloc((size_t)size + 1);
    if (serialized == NULL) return NULL;

    snprintf(serialized, (size_t)size + 1,
             "HTTP/1.1 %d %s\r\nContent-Length: %zu\r\n\r\n%s", status, reason,
             body_len, body);

    return serialized;
}

size_t response_headers_size(JSContextRef ctx, JSObjectRef headers) {
    size_t size = 0;
    JSPropertyNameArrayRef names = JSObjectCopyPropertyNames(ctx, headers);
    size_t count = JSPropertyNameArrayGetCount(names);

    for (size_t i = 0; i < count; i++) {
        JSStringRef name = JSPropertyNameArrayGetNameAtIndex(names, i);
        JSValueRef value = JSObjectGetProperty(ctx, headers, name, NULL);
        JSValueRef name_value = JSValueMakeString(ctx, name);

        char *name_cstr = jsvalue_to_cstring(ctx, name_value);
        char *value_cstr = jsvalue_to_cstring(ctx, value);
        if (name_cstr && value_cstr && name_cstr[0] != '\0') {
            size += strlen(name_cstr) + strlen(value_cstr) + strlen(": \r\n");
        }

        free(name_cstr);
        free(value_cstr);
    }

    JSPropertyNameArrayRelease(names);
    return size;
}

char *write_response_headers(JSContextRef ctx, JSObjectRef headers,
                             char *cursor) {
    JSPropertyNameArrayRef names = JSObjectCopyPropertyNames(ctx, headers);
    size_t count = JSPropertyNameArrayGetCount(names);

    for (size_t i = 0; i < count; i++) {
        JSStringRef name = JSPropertyNameArrayGetNameAtIndex(names, i);
        JSValueRef value = JSObjectGetProperty(ctx, headers, name, NULL);
        JSValueRef name_value = JSValueMakeString(ctx, name);

        char *name_cstr = jsvalue_to_cstring(ctx, name_value);
        char *value_cstr = jsvalue_to_cstring(ctx, value);
        if (name_cstr && value_cstr && name_cstr[0] != '\0') {
            cursor += sprintf(cursor, "%s: %s\r\n", name_cstr, value_cstr);
        }

        free(name_cstr);
        free(value_cstr);
    }

    JSPropertyNameArrayRelease(names);
    return cursor;
}

char *serialize_response(JSContextRef ctx, JSObjectRef response) {
    char *body = read_response_body(ctx, response);
    if (body == NULL) return NULL;

    int status = read_response_status(ctx, response);
    const char *reason = response_reason_phrase(status);
    size_t body_len = strlen(body);
    JSObjectRef headers = read_response_headers(ctx, response);
    size_t headers_size = headers ? response_headers_size(ctx, headers) : 0;

    int status_line_size =
        snprintf(NULL, 0, "HTTP/1.1 %d %s\r\n", status, reason);
    int body_size =
        snprintf(NULL, 0, "Content-Length: %zu\r\n\r\n%s", body_len, body);
    if (status_line_size < 0 || body_size < 0) {
        free(body);
        return NULL;
    }

    size_t total_size =
        (size_t)status_line_size + headers_size + (size_t)body_size;
    char *serialized = malloc(total_size + 1);
    if (serialized == NULL) {
        free(body);
        return NULL;
    }

    char *cursor = serialized;
    cursor += sprintf(cursor, "HTTP/1.1 %d %s\r\n", status, reason);
    if (headers) {
        cursor = write_response_headers(ctx, headers, cursor);
    }
    sprintf(cursor, "Content-Length: %zu\r\n\r\n%s", body_len, body);

    free(body);
    return serialized;
}

char *dispatch_request(Runtime *rt, const char *raw, JSObjectRef handler) {
    ParsedRequest request;
    if (!parse_request_line(raw, &request)) {
        return serialize_status_response(400, "bad request\n");
    }

    JSObjectRef headers = parse_request_headers(rt->ctx, raw);
    JSObjectRef req = make_request_object(rt->ctx, &request, headers);
    destroy_parsed_request(&request);

    JSValueRef args[] = {(JSValueRef)req};
    JSValueRef exception = NULL;
    JSValueRef result =
        JSObjectCallAsFunction(rt->ctx, handler, NULL, 1, args, &exception);

    if (exception || result == NULL || !is_response(rt, rt->ctx, result)) {
        return serialize_status_response(500, "internal server error\n");
    }

    JSObjectRef response = JSValueToObject(rt->ctx, result, NULL);
    if (response == NULL) {
        return serialize_status_response(500, "internal server error\n");
    }

    char *serialized = serialize_response(rt->ctx, response);
    if (serialized == NULL) {
        return serialize_status_response(500, "internal server error\n");
    }

    return serialized;
}

typedef struct HostClassSpec {
    const char *name;
    JSClassRef *class_ref;
    JSObjectCallAsConstructorCallback constructor;
} HostClassSpec;

void install_classes(Runtime *rt) {
    HostClassSpec classes[] = {
        {"Response", &rt->host_classes.response, response_constructor},
    };

    size_t num_classes = sizeof(classes) / sizeof(classes[0]);
    for (size_t i = 0; i < num_classes; i++) {
        JSClassDefinition def = kJSClassDefinitionEmpty;
        def.className = classes[i].name;
        *classes[i].class_ref = JSClassCreate(&def);

        JSObjectRef global = JSContextGetGlobalObject(rt->ctx);
        JSStringRef js_name = JSStringCreateWithUTF8CString(classes[i].name);
        JSObjectRef constructor = JSObjectMakeConstructor(
            rt->ctx, *classes[i].class_ref, classes[i].constructor);
        JSObjectSetProperty(rt->ctx, global, js_name, constructor,
                            kJSPropertyAttributeNone, NULL);
        JSStringRelease(js_name);
    }
}

void destroy_classes(Runtime *rt) {
    JSClassRef *classes[] = {
        &rt->host_classes.response,
    };

    size_t num_classes = sizeof(classes) / sizeof(classes[0]);
    for (size_t i = 0; i < num_classes; i++) {
        if (*classes[i]) {
            JSClassRelease(*classes[i]);
            *classes[i] = NULL;
        }
    }
}

JSValueRef console_log_callback(JSContextRef ctx, JSObjectRef func,
                                JSObjectRef thisObject, size_t argumentCount,
                                const JSValueRef arguments[],
                                JSValueRef *exception) {
    (void)func;
    (void)thisObject;
    (void)exception;

    for (size_t i = 0; i < argumentCount; i++) {
        print_jsvalue(ctx, arguments[i]);
        if (i < argumentCount - 1) printf(" ");
    }

    printf("\n");
    return JSValueMakeUndefined(ctx);
}

JSValueRef set_timeout_callback(JSContextRef ctx, JSObjectRef func,
                                JSObjectRef thisObject, size_t argumentCount,
                                const JSValueRef arguments[],
                                JSValueRef *exception) {
    (void)func;
    (void)thisObject;
    (void)exception;

    if (argumentCount < 2 || !JSValueIsObject(ctx, arguments[0]) ||
        !JSValueIsNumber(ctx, arguments[1])) {
        return JSValueMakeNumber(ctx, -1);
    }

    JSObjectRef callback = JSValueToObject(ctx, arguments[0], NULL);
    int delay_ms = (int)JSValueToNumber(ctx, arguments[1], NULL);
    int64_t deadline_ms = current_time_ms() + delay_ms;
    Runtime *rt = get_rt(ctx);
    int task_id = add_task(rt, callback, deadline_ms);
    return JSValueMakeNumber(ctx, task_id);
}

JSValueRef kobun_serve_callback(JSContextRef ctx, JSObjectRef func,
                                JSObjectRef thisObject, size_t argumentCount,
                                const JSValueRef arguments[],
                                JSValueRef *exception) {
    (void)func;
    (void)thisObject;
    (void)exception;

    if (argumentCount < 1 || !JSValueIsObject(ctx, arguments[0])) {
        return JSValueMakeUndefined(ctx);
    }

    JSObjectRef options = JSValueToObject(ctx, arguments[0], NULL);
    JSValueRef port_value = get_property(ctx, options, "port");
    JSValueRef fetch_value = get_property(ctx, options, "fetch");

    if (!JSValueIsNumber(ctx, port_value) ||
        !JSValueIsObject(ctx, fetch_value)) {
        return JSValueMakeUndefined(ctx);
    }

    int port = (int)JSValueToNumber(ctx, port_value, NULL);
    JSObjectRef fetch = JSValueToObject(ctx, fetch_value, NULL);
    if (port < 0 || port > 65535 || !JSObjectIsFunction(ctx, fetch)) {
        return JSValueMakeUndefined(ctx);
    }

    Runtime *rt = get_rt(ctx);
    if (!start_server(rt, port, fetch)) {
        return JSValueMakeUndefined(ctx);
    }

    return make_server_object(ctx, &rt->server);
}

JSObjectRef get_builtin_parent(Runtime *rt, const char *object_name) {
    JSObjectRef global = JSContextGetGlobalObject(rt->ctx);
    if (object_name == NULL) return global;

    JSStringRef js_object_name = JSStringCreateWithUTF8CString(object_name);
    JSValueRef value =
        JSObjectGetProperty(rt->ctx, global, js_object_name, NULL);

    if (JSValueIsObject(rt->ctx, value)) {
        JSStringRelease(js_object_name);
        return JSValueToObject(rt->ctx, value, NULL);
    }

    JSObjectRef object = JSObjectMake(rt->ctx, NULL, NULL);
    JSObjectSetProperty(rt->ctx, global, js_object_name, object,
                        kJSPropertyAttributeNone, NULL);
    JSStringRelease(js_object_name);
    return object;
}

void install_builtin(Runtime *rt, const char *object_name,
                     const char *function_name,
                     JSObjectCallAsFunctionCallback callback) {
    JSObjectRef parent = get_builtin_parent(rt, object_name);
    JSStringRef js_name = JSStringCreateWithUTF8CString(function_name);
    JSObjectRef func =
        JSObjectMakeFunctionWithCallback(rt->ctx, js_name, callback);
    JSObjectSetProperty(rt->ctx, parent, js_name, func,
                        kJSPropertyAttributeNone, NULL);
    JSStringRelease(js_name);
}

static struct {
    const char *object_name;
    const char *function_name;
    JSObjectCallAsFunctionCallback callback;
} builtins[] = {
    {NULL, "setTimeout", set_timeout_callback},
    {"console", "log", console_log_callback},
    {"Kobun", "serve", kobun_serve_callback},
};

void install_builtins(Runtime *rt) {
    size_t num_builtins = sizeof(builtins) / sizeof(builtins[0]);
    for (size_t i = 0; i < num_builtins; i++) {
        install_builtin(rt, builtins[i].object_name, builtins[i].function_name,
                        builtins[i].callback);
    }
}

// ==== interfaces ====

void setup_rt(Runtime *rt) {
    init_rt(rt);
    create_context(rt);
    attach_runtime(rt);
    install_classes(rt);
    install_builtins(rt);
}

void destroy_rt(Runtime *rt) {
    destroy_pending_tasks(rt);
    destroy_server(rt, &rt->server);

    if (rt->ctx) {
        JSGlobalContextRelease(rt->ctx);
        rt->ctx = NULL;
    }

    destroy_classes(rt);
}

bool event_loop(Runtime *rt) {
    while (!tq_is_empty(rt)) {
        if (may_consume_task(rt) < 0) return false;
        int64_t next = tq_next_deadline(rt);
        if (next >= 0) {
            sleep_until(next);
        }
    }

    return true;
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: kobun <file.js>\n");
        return 1;
    }

    char *src = read_file(argv[1]);
    if (src == NULL) {
        fprintf(stderr, "Could not open file: %s\n", argv[1]);
        return 1;
    }

    Runtime rt;
    setup_rt(&rt);

    JSStringRef script = JSStringCreateWithUTF8CString(src);
    JSValueRef exception = NULL;
    JSEvaluateScript(rt.ctx, script, NULL, NULL, 1, &exception);

    int exit_code = 0;

    if (exception) {
        printf("Error: ");
        println_jsvalue(rt.ctx, exception);
        exit_code = 1;
    } else if (!event_loop(&rt)) {
        exit_code = 1;
    }

    JSStringRelease(script);
    destroy_rt(&rt);
    free(src);
    return exit_code;
}
