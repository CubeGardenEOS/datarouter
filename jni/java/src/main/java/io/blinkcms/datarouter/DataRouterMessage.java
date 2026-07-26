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
        if (messageType < 0 || messageType > 0xff || flags < 0 || flags > 0xff) {
            throw new IllegalArgumentException("messageType and flags must fit in an unsigned byte");
        }
        this.messageType = messageType;
        this.flags = flags;
        this.varData = Arrays.copyOf(Objects.requireNonNull(varData, "varData"), varData.length);
        this.body = Arrays.copyOf(Objects.requireNonNull(body, "body"), body.length);
    }

    public int messageType() { return messageType; }
    public int flags() { return flags; }
    public byte[] varData() { return Arrays.copyOf(varData, varData.length); }
    public byte[] body() { return Arrays.copyOf(body, body.length); }
}
