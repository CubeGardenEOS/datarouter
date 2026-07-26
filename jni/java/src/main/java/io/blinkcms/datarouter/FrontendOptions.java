package io.blinkcms.datarouter;

import java.util.Objects;

/** Connection and bounded-queue limits for one independently owned frontend. */
public record FrontendOptions(
        String sharedMemoryName,
        String serviceName,
        int connectTimeoutMillis,
        int receiveTimeoutMillis,
        int acknowledgementTimeoutMillis,
        int readerWaitMillis,
        int applicationQueueCapacity,
        int maxMessageBytes,
        int maxPendingAcknowledgements,
        int chunkTtlMillis,
        int maxReassemblyGroups,
        int maxReassemblyBytes) {
    public FrontendOptions {
        sharedMemoryName = requireName(sharedMemoryName, "sharedMemoryName");
        serviceName = requireName(serviceName, "serviceName");
        positive(connectTimeoutMillis, "connectTimeoutMillis");
        positive(receiveTimeoutMillis, "receiveTimeoutMillis");
        positive(acknowledgementTimeoutMillis, "acknowledgementTimeoutMillis");
        positive(readerWaitMillis, "readerWaitMillis");
        positive(applicationQueueCapacity, "applicationQueueCapacity");
        positive(maxMessageBytes, "maxMessageBytes");
        positive(maxPendingAcknowledgements, "maxPendingAcknowledgements");
        positive(chunkTtlMillis, "chunkTtlMillis");
        positive(maxReassemblyGroups, "maxReassemblyGroups");
        positive(maxReassemblyBytes, "maxReassemblyBytes");
    }

    public static FrontendOptions defaults(String sharedMemoryName, String serviceName) {
        return new FrontendOptions(sharedMemoryName, serviceName, 3_000, 3_000, 2_000, 100,
                256, 64 * 1024 * 1024, 4_096, 30_000, 1_024, 128 * 1024 * 1024);
    }

    private static String requireName(String value, String field) {
        Objects.requireNonNull(value, field);
        if (value.isBlank() || value.indexOf('\0') >= 0) throw new IllegalArgumentException(field + " must be non-empty and NUL-free");
        return value;
    }

    private static void positive(int value, String field) {
        if (value <= 0) throw new IllegalArgumentException(field + " must be positive");
    }
}
