package io.blinkcms.datarouter;

import java.nio.ByteBuffer;
import java.util.Arrays;
import java.util.Objects;

/** One decoded DataRouter protocol message; gameplay uses MSG_CONTROL and empty TLV data. */
public final class DataRouterMessage {
    private final int messageType;
    private final int flags;
    private final byte[] varData;
    // body is either materialized eagerly (trusted byte[] handed in by the
    // JNI) or lazily from a read-only direct ByteBuffer handed in by the
    // zero-copy receive path. `body` is null only while the body still lives
    // in the direct buffer.
    private byte[] body;
    private ByteBuffer bodyDirect;
    // Epoch of the native per-thread receive buffer at the moment this
    // zero-copy message was received. bodyUnsafe() re-checks it before
    // materializing so a stale or cross-thread access fails loudly instead
    // of returning the next frame's bytes (or dereferencing a reallocated
    // buffer).
    private final long bodyEpoch;

    public DataRouterMessage(int messageType, int flags, byte[] varData, byte[] body) {
        this(messageType, flags, varData, body, false);
    }

    /**
     * Internal constructor used by the JNI receive path (and other native
     * bindings that hand in freshly allocated arrays). When {@code trustArrays}
     * is true the given arrays are stored WITHOUT a defensive copy and must
     * not be reused by the caller afterwards. Application code should keep
     * using the 4-argument constructor.
     */
    DataRouterMessage(int messageType, int flags, byte[] varData, byte[] body, boolean trustArrays) {
        if (messageType < 0 || messageType > 0xff || flags < 0 || flags > 0xff) {
            throw new IllegalArgumentException("messageType and flags must fit in an unsigned byte");
        }
        this.messageType = messageType;
        this.flags = flags;
        if (trustArrays) {
            this.varData = Objects.requireNonNull(varData, "varData");
            this.body = Objects.requireNonNull(body, "body");
        } else {
            this.varData = Arrays.copyOf(Objects.requireNonNull(varData, "varData"), varData.length);
            this.body = Arrays.copyOf(Objects.requireNonNull(body, "body"), body.length);
        }
        this.bodyDirect = null;
        this.bodyEpoch = 0; // eager bodies never consult the epoch
    }

    /**
     * Internal zero-copy constructor used by the JNI receive path. The body
     * is a read-only DirectByteBuffer view over the native receive buffer;
     * it is valid only until the next receive on the same thread, and is
     * materialized into an owned byte[] on first access via
     * {@link #bodyUnsafe()} / {@link #body()}. Any access after the next
     * receive on the receiving thread (or from another thread) throws
     * {@link IllegalStateException}. Application code should keep using the
     * byte[] constructors.
     */
    DataRouterMessage(int messageType, int flags, byte[] varData, ByteBuffer bodyDirect, long bodyEpoch) {
        if (messageType < 0 || messageType > 0xff || flags < 0 || flags > 0xff) {
            throw new IllegalArgumentException("messageType and flags must fit in an unsigned byte");
        }
        this.messageType = messageType;
        this.flags = flags;
        this.varData = Objects.requireNonNull(varData, "varData");
        this.body = null;
        this.bodyDirect = Objects.requireNonNull(bodyDirect, "bodyDirect").asReadOnlyBuffer();
        this.bodyEpoch = bodyEpoch;
    }

    public int messageType() { return messageType; }
    public int flags() { return flags; }
    public byte[] varData() { return Arrays.copyOf(varData, varData.length); }
    public byte[] body() {
        byte[] b = bodyUnsafe();
        return Arrays.copyOf(b, b.length);
    }

    /**
     * Backing array accessors for the JNI layer (and other in-package
     * native bindings). They return the internal arrays WITHOUT a defensive
     * copy, avoiding one full-body allocation + copy on every hot-path send
     * (a large part of the large-payload latency). Callers must not mutate
     * the returned arrays; application code should keep using the copying
     * {@link #varData()} / {@link #body()} getters.
     */
    byte[] varDataUnsafe() { return varData; }
    byte[] bodyUnsafe() {
        if (body == null) {
            ByteBuffer direct = bodyDirect;
            if (direct != null) {
                long current = nativeCurrentReceiveEpoch();
                if (current != bodyEpoch) {
                    throw new IllegalStateException(
                        "zero-copy body is no longer valid: the receive buffer was "
                            + "overwritten by a subsequent receive on this thread, the "
                            + "message was received on a different thread, or the "
                            + "receiving thread exited; materialize body()/bodyUnsafe() "
                            + "or send() before the next receive on the same thread");
                }
                byte[] materialized = new byte[direct.remaining()];
                direct.duplicate().get(materialized);
                body = materialized;
                bodyDirect = null;
            } else {
                body = new byte[0];
            }
        }
        return body;
    }

    /**
     * @return the calling thread's native receive-buffer epoch, used to
     *     detect stale / cross-thread zero-copy bodies (see
     *     {@link #bodyUnsafe()}).
     */
    private static native long nativeCurrentReceiveEpoch();
}
