// KoBun is a simple JS runtime written in C using JavaScriptCore.
// Copyright (c) 2026 Yuchi Yamaguchi under the MIT License.

#include <JavaScriptCore/JavaScriptCore.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

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
} Runtime;

int gen_task_id(Runtime *rt) {
    rt->_task_id_counter++;
    return rt->_task_id_counter;
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

void print_jsvalue(JSContextRef ctx, JSValueRef value) {
    JSStringRef str = JSValueToStringCopy(ctx, value, NULL);
    size_t size = JSStringGetMaximumUTF8CStringSize(str);
    char *cstr = malloc(size);

    JSStringGetUTF8CString(str, cstr, size);
    printf("%s", cstr);

    free(cstr);
    JSStringRelease(str);
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

// ==== Runtime management ====

void init_rt(Runtime *rt) {
    rt->ctx = NULL;
    rt->host_classes.response = NULL;
    rt->task_queue_head = NULL;
    rt->task_queue_tail = NULL;
    rt->_task_id_counter = 0;
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

    return response;
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
