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
    fprintf(out, "%s\n", cstr);

    free(cstr);
    JSStringRelease(str);
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
    JSStringRef script = JSStringCreateWithUTF8CString(src);
    JSValueRef exception = NULL;
    JSValueRef result = JSEvaluateScript(context, script, NULL, NULL, 1, &exception);

    if (exception) {
        fprintf(stderr, "Error: ");
        print_jsvalue(stderr, context, exception);
    } else {
        print_jsvalue(stdout, context, result);
    }

    JSStringRelease(script);
    JSGlobalContextRelease(context);
    free(src);
    return exception ? 1 : 0;
}
