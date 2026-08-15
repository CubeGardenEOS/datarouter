package io.blinkcms.datarouter;

import java.time.Duration;
import java.util.Objects;
import java.util.Optional;
import java.util.concurrent.atomic.AtomicLong;

/**
 * Java 17 frontend for a switch-owned DataRouter mapping. Native calls never
 * call into the JVM from the reader thread; callers own receive scheduling.
 */
public final class DataRouterFrontend implements AutoCloseable {
    private final AtomicLong handle;

    private DataRouterFrontend(long handle) {
        this.handle = new AtomicLong(handle);
    }

    public static DataRouterFrontend open(FrontendOptions options) {
        Objects.requireNonNull(options, "options");
        return new DataRouterFrontend(nativeOpen(options));
    }

    public void send(String targetService, DataRouterMessage message, Duration timeout) {
        Objects.requireNonNull(targetService, "targetService");
        Objects.requireNonNull(message, "message");
        nativeSend(requireHandle(), targetService, message, timeoutMillis(timeout));
    }

    public ReceivedDataRouterMessage receive(Duration timeout) {
        return nativeReceive(requireHandle(), timeoutMillis(timeout), false);
    }

    /**
     * Zero-copy receive: identical to {@link #receive} except the message
     * body is a DirectByteBuffer view over the native receive buffer instead
     * of a freshly allocated byte[]. The view is valid only until the next
     * receive on the same thread; {@code message.body()} / {@code message
     * .bodyUnsafe()} materialize the payload into an owned byte[] on first
     * access. Use this on hot paths that consume the payload before the next
     * receive (the same contract as the C# binding's preallocated receive
     * buffer) to avoid a JVM heap allocation per large message.
     */
    public ReceivedDataRouterMessage receiveZeroCopy(Duration timeout) {
        return nativeReceiveZeroCopy(requireHandle(), timeoutMillis(timeout), false);
    }

    public Optional<ReceivedDataRouterMessage> tryReceive() {
        return Optional.ofNullable(nativeReceive(requireHandle(), 1, true));
    }

    public DataRouterStats stats() {
        return nativeStats(requireHandle());
    }

    public RegistrySnapshot registrySnapshot() {
        return nativeRegistrySnapshot(requireHandle());
    }

    public static DataRouterCompatibility compatibility() {
        return nativeCompatibility();
    }

    @Override
    public void close() {
        long current = handle.getAndSet(0L);
        if (current != 0L) nativeClose(current);
    }

    private long requireHandle() {
        long current = handle.get();
        if (current == 0L) throw new IllegalStateException("DataRouterFrontend is closed");
        return current;
    }

    private static long timeoutMillis(Duration timeout) {
        Objects.requireNonNull(timeout, "timeout");
        if (timeout.isNegative() || timeout.isZero() || timeout.toMillis() == 0L || timeout.toMillis() > Integer.MAX_VALUE) {
            throw new IllegalArgumentException("timeout must be 1 ms through Integer.MAX_VALUE ms");
        }
        return timeout.toMillis();
    }

    private static native long nativeOpen(FrontendOptions options);
    private static native void nativeSend(long handle, String targetService, DataRouterMessage message, long timeoutMillis);
    private static native ReceivedDataRouterMessage nativeReceive(long handle, long timeoutMillis, boolean allowEmpty);
    private static native ReceivedDataRouterMessage nativeReceiveZeroCopy(long handle, long timeoutMillis, boolean allowEmpty);
    private static native DataRouterStats nativeStats(long handle);
    private static native RegistrySnapshot nativeRegistrySnapshot(long handle);
    private static native DataRouterCompatibility nativeCompatibility();
    private static native void nativeClose(long handle);
}
