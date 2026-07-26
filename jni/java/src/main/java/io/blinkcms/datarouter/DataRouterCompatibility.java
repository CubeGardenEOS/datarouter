package io.blinkcms.datarouter;

/** Fixed compatibility identity validated before a native transport is accepted. */
public record DataRouterCompatibility(
        String crateSemver, int layoutVersion, int frontendAbi, int jniAbi,
        int channelSize, String abiFingerprint) {}
