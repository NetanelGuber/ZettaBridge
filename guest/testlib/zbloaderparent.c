extern void zb_leaf_bind(int*);
extern int zb_leaf_value(void);
extern int zb_versioned(void);
extern int zb_missing_weak(void) __attribute__((weak));

static int constructed;
static int* events;

__attribute__((constructor)) static void parent_init(void) { constructed = 11; }
__attribute__((destructor)) static void parent_fini(void) {
    if (events != 0) *events |= 1;
}

void zb_parent_bind(int* value) {
    events = value;
    zb_leaf_bind(value);
}
int zb_parent_value(void) { return constructed + zb_leaf_value(); }
int zb_parent_versioned(void) { return zb_versioned(); }
int zb_parent_weak_is_null(void) { return zb_missing_weak == 0; }
