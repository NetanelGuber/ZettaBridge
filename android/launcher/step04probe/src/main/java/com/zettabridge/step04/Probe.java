package com.zettabridge.step04;

final class Probe {
    static int callbacks;
    private Probe() {}

    static int onGuestLoaded(int value) {
        callbacks++;
        return value * 2;
    }

    static native int add(int value);
    static native int multiply(int value);
    static native int subtract(int value);
}
