package io.blinkcms.datarouter;

/** Counters are monotonic for this frontend or its current switch mapping. */
public record DataRouterStats(
        long sentFrames, long sentChunks, long receivedFrames, long receivedComplete,
        long receivedChunkAcknowledgements, long receivedChunkErrors, long acknowledgementTimeouts,
        long queueFull, long protocolErrors, long droppedFrames, long pendingAcknowledgements,
        long queuedMessages, long readerAlive, long readerErrors, long keepaliveTicks,
        long lastKeepaliveNanos, long reassemblyExpired, long reassemblyRejected,
        long corruptedRings, long registrationRecoveries, long blockedWakeups,
        long deadProcessReaps, long transportGeneration, long switchRecoveries) {}
