package io.blinkcms.datarouter;

/** Stable native DataRouter failure. {@link #code()} is a C transport code. */
public final class DataRouterException extends RuntimeException {
    private final int code;

    public DataRouterException(int code, String message) {
        super(message);
        this.code = code;
    }

    public int code() {
        return code;
    }
}
