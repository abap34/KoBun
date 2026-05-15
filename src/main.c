#include <JavaScriptCore/JavaScriptCore.h>
#include <stdio.h>
#include <stdlib.h>

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

void print_jsvalue(FILE *out, JSContextRef context, JSValueRef value) {
    JSStringRef str = JSValueToStringCopy(context, value, NULL);
    size_t size = JSStringGetMaximumUTF8CStringSize(str);
    char *cstr = malloc(size);

    JSStringGetUTF8CString(str, cstr, size);
    fprintf(out, "%s", cstr);

    free(cstr);
    JSStringRelease(str);
}

void println_jsvalue(FILE *out, JSContextRef context, JSValueRef value) {
    print_jsvalue(out, context, value);
    fprintf(out, "\n");
}

JSValueRef console_log_callback(JSContextRef ctx, JSObjectRef func,
                                JSObjectRef thisObject, size_t argumentCount,
                                const JSValueRef arguments[],
                                JSValueRef *exception) {
    (void)func;
    (void)thisObject;
    (void)exception;

    for (size_t i = 0; i < argumentCount; i++) {
        print_jsvalue(stdout, ctx, arguments[i]);
        if (i < argumentCount - 1)
            fprintf(stdout, " ");
    }

    fprintf(stdout, "\n");
    return JSValueMakeUndefined(ctx);
}

void setup_ctx(JSGlobalContextRef ctx) {
    JSObjectRef global = JSContextGetGlobalObject(ctx);

    JSObjectRef console = JSObjectMake(ctx, NULL, NULL);
    JSStringRef console_name = JSStringCreateWithUTF8CString("console");
    JSObjectSetProperty(ctx, global, console_name, console,
                        kJSPropertyAttributeNone, NULL);

    JSStringRef log_name = JSStringCreateWithUTF8CString("log");
    JSObjectRef log_func =
        JSObjectMakeFunctionWithCallback(ctx, log_name, console_log_callback);
    JSObjectSetProperty(ctx, console, log_name, log_func,
                        kJSPropertyAttributeNone, NULL);

    JSStringRelease(log_name);
    JSStringRelease(console_name);
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

    JSGlobalContextRef context = JSGlobalContextCreate(NULL);

    setup_ctx(context);

    JSStringRef script = JSStringCreateWithUTF8CString(src);
    JSValueRef exception = NULL;
    JSEvaluateScript(context, script, NULL, NULL, 1, &exception);

    if (exception) {
        fprintf(stderr, "Error: ");
        println_jsvalue(stderr, context, exception);
    }

    JSStringRelease(script);
    JSGlobalContextRelease(context);
    free(src);
    return exception ? 1 : 0;
}
