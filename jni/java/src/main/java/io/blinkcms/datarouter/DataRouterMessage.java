package io.blinkcms.datarouter;

import java.util.Arrays;
import java.util.Objects;

/** One decoded DataRouter protocol message; gameplay uses MSG_CONTROL and empty TLV data. */
public final class DataRouterMessage {
    private final int messageType;
    private final int flags;
    private final byte[] varData;
    private final byte[] body;

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
    }

    public int messageType() { return messageType; }
    public int flags() { return flags; }
    public byte[] varData() { return Arrays.copyOf(varData, varData.length); }
    public byte[] body() { return Arrays.copyOf(body, body.length); }

    /**
     * Backing array accessors for the JNI layer (and other in-package
     * native bindings). They return the internal arrays WITHOUT a defensive
     * copy, avoiding one full-body allocation + copy on every hot-path send
     * (a large part of the large-payload latency). Callers must not mutate
     * the returned arrays; application code should keep using the copying
     * {@link #varData()} / {@link #body()} getters.
     */
    public byte[] varDataUnsafe() { return varData; }
    public byte[] bodyUnsafe() { return body; }
}
