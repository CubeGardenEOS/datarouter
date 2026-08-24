package io.blinkcms.datarouter;

/** Stable native DataRouter failure. {@link #code()} is a C transport code. */
public final class DataRouterException extends RuntimeException {
    private final int code;

    public DataRouterException(String message) {
        super(message);
        this.code = parseCode(message);
    }

    public DataRouterException(int code, String message) {
        super(message);
        this.code = code;
    }

    private static int parseCode(String message) {
        if (message != null && message.startsWith("DataRouter native error ")) {
            try {
                int colon = message.indexOf(':', 24);
                if (colon > 24) {
                    return Integer.parseInt(message.substring(24, colon).trim());
                }
            } catch (Exception ignored) {
            }
        }
        return -1;
    }

    public int code() {
        return code;
    }
}
