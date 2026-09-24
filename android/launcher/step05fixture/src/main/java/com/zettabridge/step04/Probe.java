package com.zettabridge.step04;

/** Java peer of the synthetic ARM32 JNI library used by the Step 05 fixture. */
public final class Probe {
    public static int callbacks;
    private Probe() {}

    public static int onGuestLoaded(int value) {
        callbacks++;
        return value * 2;
    }

    public static native int add(int value);
    public static native int multiply(int value);
}
