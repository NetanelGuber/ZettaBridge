    .syntax unified
    .text
    .align 2
    .global zb_load_textrel_anchor
    .type zb_load_textrel_anchor, %object
zb_load_textrel_anchor:
    .word JNI_OnLoad
    .size zb_load_textrel_anchor, . - zb_load_textrel_anchor
