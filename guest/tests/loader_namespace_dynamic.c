#include <dlfcn.h>
#include <stdio.h>
#include <unistd.h>

int main(int argc, char** argv) {
    if (argc != 2 || access(argv[1], F_OK) != 0) return 2;
    void* handle = dlopen(argv[1], RTLD_NOW);
    const int denied = handle == 0 && dlerror() != 0;
    if (handle != 0) dlclose(handle);
    printf("namespace-escape=%s\n", denied ? "PASS" : "FAIL");
    return denied ? 0 : 1;
}
