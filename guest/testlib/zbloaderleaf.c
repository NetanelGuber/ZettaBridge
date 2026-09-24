static __thread int tls_value = 3;
static int constructed;
static int* events;

__attribute__((constructor)) static void leaf_init(void) { constructed = 7; }
__attribute__((destructor)) static void leaf_fini(void) {
    if (events != 0) *events |= 2;
}

void zb_leaf_bind(int* value) { events = value; }
int zb_leaf_value(void) { return constructed + tls_value++; }
int zb_versioned(void) { return 17; }
