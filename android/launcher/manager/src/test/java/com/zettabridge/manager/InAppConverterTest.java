package com.zettabridge.manager;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertThrows;

import org.junit.Test;

import java.io.IOException;
import java.util.Arrays;
import java.util.Map;

public class InAppConverterTest {
    private static InAppConverter.Library lib(String abi, String name) {
        int rank = "armeabi-v7a".equals(abi) ? 1 : "armeabi".equals(abi) ? 0 : -1;
        return new InAppConverter.Library("lib/" + abi + "/" + name, abi, rank, 1024);
    }

    @Test public void matchingX86CopiesSelectArm32Guests() throws Exception {
        Map<String, InAppConverter.Library> selected = InAppConverter.selectGuestLibraries(Arrays.asList(
                lib("x86", "libapp.so"), lib("armeabi", "libapp.so"),
                lib("armeabi-v7a", "libapp.so"), lib("x86", "libaudio.so"),
                lib("armeabi-v7a", "libaudio.so")));
        assertEquals(2, selected.size());
        assertEquals("lib/armeabi-v7a/libapp.so", selected.get("libapp.so").path);
        assertEquals("lib/armeabi-v7a/libaudio.so", selected.get("libaudio.so").path);
    }

    @Test public void x86OnlyLibraryFailsClosed() {
        IOException error = assertThrows(IOException.class, () -> InAppConverter.selectGuestLibraries(Arrays.asList(
                lib("armeabi-v7a", "libapp.so"), lib("x86", "libextra.so"))));
        assertEquals("x86 library has no ARM32 counterpart: libextra.so", error.getMessage());
    }

    @Test public void duplicateArm32VariantFailsClosed() {
        IOException error = assertThrows(IOException.class, () -> InAppConverter.selectGuestLibraries(Arrays.asList(
                lib("armeabi-v7a", "libapp.so"), lib("armeabi-v7a", "libapp.so"))));
        assertEquals("duplicate guest library at the same ABI: libapp.so", error.getMessage());
    }
}
