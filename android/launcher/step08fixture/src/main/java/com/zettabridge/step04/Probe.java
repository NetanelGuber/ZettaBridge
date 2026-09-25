package com.zettabridge.step04;

/** Synthetic Java peer used by libzbstep04a.so in both app processes. */
public final class Probe {
    public static int callbacks;
    private Probe() {}
    public static int onGuestLoaded(int value) { callbacks++; return value * 2; }
    public static native int add(int value);
    public static native int multiply(int value);
}
