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
    // JNI) or lazily from a direct ByteBuffer handed in by the zero-copy
    // receive path. `body` is null only while the body still lives in the
    // direct buffer.
    private byte[] body;
    private ByteBuffer bodyDirect;

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
    public DataRouterMessage(int messageType, int flags, byte[] varData, byte[] body, boolean trustArrays) {
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
    }

    /**
     * Internal zero-copy constructor used by the JNI receive path. The body
     * is a DirectByteBuffer view over the native receive buffer; it is valid
     * only until the next receive on the same thread, and is materialized
     * into an owned byte[] on first access via {@link #bodyUnsafe()} /
     * {@link #body()}. Application code should keep using the byte[]
     * constructors.
     */
    public DataRouterMessage(int messageType, int flags, byte[] varData, ByteBuffer bodyDirect) {
        if (messageType < 0 || messageType > 0xff || flags < 0 || flags > 0xff) {
            throw new IllegalArgumentException("messageType and flags must fit in an unsigned byte");
        }
        this.messageType = messageType;
        this.flags = flags;
        this.varData = Objects.requireNonNull(varData, "varData");
        this.body = null;
        this.bodyDirect = Objects.requireNonNull(bodyDirect, "bodyDirect");
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
    public byte[] varDataUnsafe() { return varData; }
    public byte[] bodyUnsafe() {
        if (body == null) {
            ByteBuffer direct = bodyDirect;
            if (direct != null) {
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
}
