package io.blinkcms.datarouter;

import java.time.Duration;
import java.util.concurrent.atomic.AtomicReference;

/**
 * Zero-copy receive lifecycle tests: normal materialization works, stale
 * (post-receive) and cross-thread body access fails loudly, and the failure
 * propagates through the JNI send path.
 *
 * <p>Run via {@code jni/run_java_tests.sh} (needs a live switch on the shm
 * name passed as the first argument). Exits non-zero on any failure.
 */
public final class ZcLifecycleTest {

    static {
        System.loadLibrary("blinkcms_datarouter_jni");
    }

    private static int failures = 0;

    public static void main(String[] args) throws Exception {
        if (args.length < 1) {
            System.err.println("usage: ZcLifecycleTest <shm-name>");
            System.exit(2);
        }
        String shm = args[0];
        DataRouterFrontend portal = DataRouterFrontend.open(FrontendOptions.defaults(shm, "portal"));
        DataRouterFrontend core = DataRouterFrontend.open(FrontendOptions.defaults(shm, "core"));
        Duration d = Duration.ofMillis(3000);

        byte[] body = new byte[4096];
        for (int i = 0; i < body.length; i++) body[i] = (byte) (i & 0xff);

        // (a) normal zero-copy materialization returns the exact bytes
        portal.send("core", new DataRouterMessage(0x07, 0, new byte[0], body), d);
        ReceivedDataRouterMessage m1 = core.receiveZeroCopy(d);
        byte[] got = m1.message().body();
        check("a-normal-materialize", java.util.Arrays.equals(got, body));
        check("a-source", m1.source().equals("portal"));

        // (b) body access after a subsequent receive on the same thread throws
        portal.send("core", new DataRouterMessage(0x07, 0, new byte[0], new byte[]{1}), d);
        ReceivedDataRouterMessage stale = core.receiveZeroCopy(d);
        portal.send("core", new DataRouterMessage(0x07, 0, new byte[0], new byte[]{2}), d);
        ReceivedDataRouterMessage next = core.receiveZeroCopy(d);
        checkThrows("b-stale-body-throws", IllegalStateException.class,
                () -> messageBody(stale));

        // (c) sending a stale zero-copy message propagates the failure (the
        // JNI send path materializes the body via bodyUnsafe())
        checkThrows("c-stale-send-throws", IllegalStateException.class,
                () -> portal.send("core", stale.message(), d));

        // (d) cross-thread body access throws on the other thread
        AtomicReference<Throwable> cross = new AtomicReference<>();
        Thread t = new Thread(() -> {
            try {
                next.message().body();
            } catch (Throwable ex) {
                cross.set(ex);
            }
        });
        t.start();
        t.join(5000);
        check("d-cross-thread-throws", cross.get() instanceof IllegalStateException);

        // (d2) the receiving thread can still materialize `next` (epoch unchanged)
        byte[] n2 = next.message().body();
        check("d2-own-thread-ok", n2.length == 1 && n2[0] == 2);

        // (e) eager receive() is unaffected by zero-copy invalidation
        portal.send("core", new DataRouterMessage(0x07, 0, new byte[0], body), d);
        ReceivedDataRouterMessage m2 = core.receive(d);
        check("e-eager-body", java.util.Arrays.equals(m2.message().body(), body));

        portal.close();
        core.close();
        if (failures == 0) {
            System.out.println("ZcLifecycleTest: ALL PASS");
        } else {
            System.out.println("ZcLifecycleTest: " + failures + " FAILURE(S)");
            System.exit(1);
        }
    }

    private static byte[] messageBody(ReceivedDataRouterMessage m) {
        return m.message().body();
    }

    private static void check(String name, boolean ok) {
        System.out.println((ok ? "PASS " : "FAIL ") + name);
        if (!ok) failures++;
    }

    private static void checkThrows(String name, Class<? extends Throwable> type, Runnable r) {
        try {
            r.run();
            System.out.println("FAIL " + name + " (no exception thrown)");
            failures++;
        } catch (Throwable ex) {
            if (type.isInstance(ex)) {
                System.out.println("PASS " + name);
            } else {
                System.out.println("FAIL " + name + " (expected " + type.getSimpleName()
                        + ", got " + ex.getClass().getSimpleName() + ": " + ex.getMessage() + ")");
                failures++;
            }
        }
    }
}
