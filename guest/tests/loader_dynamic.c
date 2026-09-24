#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

static int line(const char* name, int ok) {
    printf("%s=%s\n", name, ok ? "PASS" : "FAIL");
    return ok;
}

int main(void) {
    int ok = 1;
    void* missing = dlopen("libzbloaderabsent.so", RTLD_NOW);
    ok &= line("missing-library", missing == 0 && dlerror() != 0);
    void* absent = dlopen("libzbloaderparent.so", RTLD_NOW | RTLD_NOLOAD);
    ok &= line("noload-before", absent == 0);
    if (absent != 0) dlclose(absent);

    int events = 0;
    void* parent = dlopen("libzbloaderparent.so", RTLD_NOW | RTLD_LOCAL);
    if (parent == 0) {
        fprintf(stderr, "loader fixture dlopen: %s\n", dlerror());
        return 1;
    }
    void (*bind)(int*) = (void (*)(int*))dlsym(parent, "zb_parent_bind");
    int (*value)(void) = (int (*)(void))dlsym(parent, "zb_parent_value");
    int (*weak)(void) = (int (*)(void))dlsym(parent, "zb_parent_weak_is_null");
    ok &= line("dependencies-and-constructors", bind && value && (bind(&events), value() == 21));
    ok &= line("weak-symbol", weak && weak());
    Dl_info info = {0};
    ok &= line("dladdr", value && dladdr((void*)value, &info) && info.dli_fname &&
                         strstr(info.dli_fname, "libzbloaderparent.so") != 0);
    int (*versioned)(void) = (int (*)(void))dlsym(parent, "zb_parent_versioned");
    ok &= line("versioned-symbol", versioned && versioned() == 17);
    ok &= line("missing-symbol", dlsym(parent, "zb_does_not_exist") == 0 && dlerror() != 0);
    void* loaded = dlopen("libzbloaderparent.so", RTLD_NOW | RTLD_NOLOAD);
    ok &= line("noload-after", loaded != 0);
    if (loaded != 0) dlclose(loaded);
    ok &= line("dlclose", dlclose(parent) == 0 && events == 3);

    void* lazy = dlopen("libzbloaderparent.so", RTLD_LAZY | RTLD_LOCAL);
    ok &= line("lazy-binding", lazy != 0);
    if (lazy != 0) dlclose(lazy);
    void* global = dlopen("libzbloaderleaf.so", RTLD_NOW | RTLD_GLOBAL);
    ok &= line("global-binding", global != 0 && dlsym(RTLD_DEFAULT, "zb_versioned") != 0);
    if (global != 0) dlclose(global);
    static int nodelete_events;
    void* nodelete = dlopen("libzbloaderleaf.so", RTLD_NOW | RTLD_NODELETE);
    if (nodelete != 0) {
        void (*leaf_bind)(int*) = (void (*)(int*))dlsym(nodelete, "zb_leaf_bind");
        if (leaf_bind != 0) leaf_bind(&nodelete_events);
        dlclose(nodelete);
    }
    void* retained = dlopen("libzbloaderleaf.so", RTLD_NOW | RTLD_NOLOAD);
    ok &= line("nodelete", nodelete != 0 && retained != 0 && nodelete_events == 0);
    if (retained != 0) dlclose(retained);

    void* bad = dlopen("libzbloaderbad.so", RTLD_NOW);
    ok &= line("relocation-error", bad == 0 && dlerror() != 0);
    if (bad != 0) dlclose(bad);
    return ok ? 0 : 1;
}
