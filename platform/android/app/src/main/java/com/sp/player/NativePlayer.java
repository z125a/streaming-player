package com.sp.player;

import android.view.Surface;

/**
 * JNI wrapper for the C++ streaming player core.
 * Handles lifecycle: create → open → setSurface → play → stop → destroy
 */
public class NativePlayer {
    static {
        System.loadLibrary("sp_player");
    }

    private long nativeHandle;

    public NativePlayer() {
        nativeHandle = nativeCreate();
    }

    public boolean open(String url) {
        return nativeOpen(nativeHandle, url);
    }

    public void setSurface(Surface surface) {
        nativeSetSurface(nativeHandle, surface);
    }

    public void play() {
        nativePlay(nativeHandle);
    }

    public void stop() {
        nativeStop(nativeHandle);
    }

    public boolean isLive() {
        return nativeIsLive(nativeHandle);
    }

    public void release() {
        nativeDestroy(nativeHandle);
        nativeHandle = 0;
    }

    // Native methods
    private native long nativeCreate();
    private native boolean nativeOpen(long handle, String url);
    private native void nativeSetSurface(long handle, Surface surface);
    private native void nativePlay(long handle);
    private native void nativeStop(long handle);
    private native void nativeDestroy(long handle);
    private native boolean nativeIsLive(long handle);
}
