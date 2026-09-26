package com.zettabridge.step04;

/** The source APK exposes only the ARM32 implementation of these methods. */
public final class Probe {
    public static int callbacks;
    public static int onGuestLoaded(int value) { callbacks++; return value * 2; }
    public static native int add(int input);
    public static native int multiply(int input);
    private Probe() {}
}
