//! Java 17 DataRouter frontend.
//!
//! The JNI boundary intentionally consumes the Rust `Frontend` API rather
//! than the raw Port ABI. Handles are monotonic, never reused, and each call
//! holds a lifecycle lease. Closing removes the handle first, rejects future
//! entries, then waits for leased calls before dropping the frontend.

use std::collections::BTreeMap;
use std::os::raw::c_int;
use std::panic::{self, AssertUnwindSafe};
use std::sync::atomic::{AtomicI64, Ordering};
use std::sync::{Arc, Condvar, Mutex, OnceLock};

use blinkcms_datarouter::frontend::{Frontend, FrontendOptions};
use blinkcms_datarouter::protocol::{self, Message};
use jni::objects::{JByteArray, JClass, JObject, JString, JValue};
use jni::sys::{jboolean, jint, jlong, jobject, JNI_TRUE};
use jni::JNIEnv;

const JNI_ABI_VERSION: jint = 1;
const LAYOUT_VERSION: jint = 9;
const FRONTEND_ABI_VERSION: jint = 2;
const ABI_FINGERPRINT: &str = "9805e860b0e73cda067a1b8b1b64904ab60d3800d6567308b7dab4e2defe2190";
const JAVA_EXCEPTION: &str = "io/blinkcms/datarouter/DataRouterException";
const MAX_TIMEOUT_MILLIS: jlong = i32::MAX as jlong;

struct Lifecycle {
    frontend: Option<Arc<Frontend>>,
    closing: bool,
    in_flight: usize,
}

struct NativeHandle {
    lifecycle: Mutex<Lifecycle>,
    drained: Condvar,
    receive_lock: Mutex<()>,
}

impl NativeHandle {
    fn new(frontend: Frontend) -> Self {
        Self {
            lifecycle: Mutex::new(Lifecycle {
                frontend: Some(Arc::new(frontend)),
                closing: false,
                in_flight: 0,
            }),
            drained: Condvar::new(),
            receive_lock: Mutex::new(()),
        }
    }

    fn enter(self: &Arc<Self>) -> Result<Lease, c_int> {
        let mut state = self
            .lifecycle
            .lock()
            .map_err(|_| blinkcms_datarouter::DATAROUTER_ERR_INTERNAL)?;
        if state.closing {
            return Err(blinkcms_datarouter::DATAROUTER_ERR_NOT_FOUND);
        }
        let frontend = state
            .frontend
            .as_ref()
            .cloned()
            .ok_or(blinkcms_datarouter::DATAROUTER_ERR_NOT_FOUND)?;
        state.in_flight = state.in_flight.saturating_add(1);
        Ok(Lease {
            owner: Arc::clone(self),
            frontend,
        })
    }

    fn leave(&self) {
        let Ok(mut state) = self.lifecycle.lock() else {
            return;
        };
        state.in_flight = state.in_flight.saturating_sub(1);
        if state.closing && state.in_flight == 0 {
            self.drained.notify_all();
        }
    }

    fn close(&self) {
        let frontend = {
            let Ok(mut state) = self.lifecycle.lock() else {
                return;
            };
            state.closing = true;
            while state.in_flight != 0 {
                state = match self.drained.wait(state) {
                    Ok(guard) => guard,
                    Err(_) => return,
                };
            }
            state.frontend.take()
        };
        drop(frontend);
    }
}

struct Lease {
    owner: Arc<NativeHandle>,
    frontend: Arc<Frontend>,
}

impl Drop for Lease {
    fn drop(&mut self) {
        self.owner.leave();
    }
}

static HANDLES: OnceLock<Mutex<BTreeMap<i64, Arc<NativeHandle>>>> = OnceLock::new();
static NEXT_HANDLE: AtomicI64 = AtomicI64::new(1);

fn handles() -> &'static Mutex<BTreeMap<i64, Arc<NativeHandle>>> {
    HANDLES.get_or_init(|| Mutex::new(BTreeMap::new()))
}

fn next_handle() -> Result<i64, c_int> {
    let value = NEXT_HANDLE.fetch_add(1, Ordering::Relaxed);
    if value <= 0 {
        return Err(blinkcms_datarouter::DATAROUTER_ERR_INTERNAL);
    }
    Ok(value)
}

fn lease(id: jlong) -> Result<Lease, c_int> {
    if id <= 0 {
        return Err(blinkcms_datarouter::DATAROUTER_ERR_INVALID_ARGUMENT);
    }
    let handle = handles()
        .lock()
        .map_err(|_| blinkcms_datarouter::DATAROUTER_ERR_INTERNAL)?
        .get(&id)
        .cloned()
        .ok_or(blinkcms_datarouter::DATAROUTER_ERR_NOT_FOUND)?;
    handle.enter()
}

fn duration_millis(value: jlong) -> Result<u32, c_int> {
    if !(1..=MAX_TIMEOUT_MILLIS).contains(&value) {
        return Err(blinkcms_datarouter::DATAROUTER_ERR_INVALID_ARGUMENT);
    }
    Ok(value as u32)
}

fn throw(env: &mut JNIEnv, code: c_int, detail: impl AsRef<str>) {
    let message = format!("DataRouter native error {code}: {}", detail.as_ref());
    let _ = env.throw_new(JAVA_EXCEPTION, message);
}

fn run<T>(env: &mut JNIEnv, operation: impl FnOnce(&mut JNIEnv) -> Result<T, c_int>) -> Option<T> {
    match panic::catch_unwind(AssertUnwindSafe(|| operation(env))) {
        Ok(Ok(value)) => Some(value),
        Ok(Err(code)) => {
            throw(env, code, blinkcms_datarouter::strerror(code).to_string_lossy());
            None
        }
        Err(_) => {
            throw(env, blinkcms_datarouter::DATAROUTER_ERR_INTERNAL, "native panic contained");
            None
        }
    }
}

fn get_string(env: &mut JNIEnv, object: JObject) -> Result<String, c_int> {
    let string = JString::from(object);
    env.get_string(&string)
        .map(|value| value.into())
        .map_err(|_| blinkcms_datarouter::DATAROUTER_ERR_INVALID_ARGUMENT)
}

fn get_option_string(env: &mut JNIEnv, options: &JObject, method: &str) -> Result<String, c_int> {
    let value = env
        .call_method(options, method, "()Ljava/lang/String;", &[])
        .map_err(|_| blinkcms_datarouter::DATAROUTER_ERR_INVALID_ARGUMENT)?
        .l()
        .map_err(|_| blinkcms_datarouter::DATAROUTER_ERR_INVALID_ARGUMENT)?;
    get_string(env, value)
}

fn get_option_int(env: &mut JNIEnv, options: &JObject, method: &str) -> Result<jint, c_int> {
    env.call_method(options, method, "()I", &[])
        .map_err(|_| blinkcms_datarouter::DATAROUTER_ERR_INVALID_ARGUMENT)?
        .i()
        .map_err(|_| blinkcms_datarouter::DATAROUTER_ERR_INVALID_ARGUMENT)
}

fn read_options(env: &mut JNIEnv, options: JObject) -> Result<FrontendOptions, c_int> {
    let options = FrontendOptions {
        shm_name: get_option_string(env, &options, "sharedMemoryName")?,
        service_name: get_option_string(env, &options, "serviceName")?,
        connect_timeout_ms: get_option_int(env, &options, "connectTimeoutMillis")? as u32,
        recv_timeout_ms: get_option_int(env, &options, "receiveTimeoutMillis")? as u32,
        ack_timeout_ms: get_option_int(env, &options, "acknowledgementTimeoutMillis")? as u32,
        reader_wait_ms: get_option_int(env, &options, "readerWaitMillis")? as u32,
        app_queue_capacity: get_option_int(env, &options, "applicationQueueCapacity")? as usize,
        max_message_bytes: get_option_int(env, &options, "maxMessageBytes")? as usize,
        max_pending_acks: get_option_int(env, &options, "maxPendingAcknowledgements")? as usize,
        chunk_ttl_ms: get_option_int(env, &options, "chunkTtlMillis")? as u32,
        max_reassembly_groups: get_option_int(env, &options, "maxReassemblyGroups")? as usize,
        max_reassembly_bytes: get_option_int(env, &options, "maxReassemblyBytes")? as usize,
    };
    options.normalized()
}

fn get_message_bytes(env: &mut JNIEnv, message: &JObject, method: &str) -> Result<Vec<u8>, c_int> {
    let object = env
        .call_method(message, method, "()[B", &[])
        .map_err(|_| blinkcms_datarouter::DATAROUTER_ERR_INVALID_ARGUMENT)?
        .l()
        .map_err(|_| blinkcms_datarouter::DATAROUTER_ERR_INVALID_ARGUMENT)?;
    env.convert_byte_array(JByteArray::from(object))
        .map_err(|_| blinkcms_datarouter::DATAROUTER_ERR_INVALID_ARGUMENT)
}

fn read_message(env: &mut JNIEnv, message: JObject) -> Result<Message, c_int> {
    let message_type = env
        .call_method(&message, "messageType", "()I", &[])
        .map_err(|_| blinkcms_datarouter::DATAROUTER_ERR_INVALID_ARGUMENT)?
        .i()
        .map_err(|_| blinkcms_datarouter::DATAROUTER_ERR_INVALID_ARGUMENT)?;
    let flags = env
        .call_method(&message, "flags", "()I", &[])
        .map_err(|_| blinkcms_datarouter::DATAROUTER_ERR_INVALID_ARGUMENT)?
        .i()
        .map_err(|_| blinkcms_datarouter::DATAROUTER_ERR_INVALID_ARGUMENT)?;
    if !(0..=255).contains(&message_type) || !(0..=255).contains(&flags) {
        return Err(blinkcms_datarouter::DATAROUTER_ERR_INVALID_ARGUMENT);
    }
    let result = Message {
        message_type: message_type as u8,
        flags: flags as u8,
        var_data: get_message_bytes(env, &message, "varData")?,
        body: get_message_bytes(env, &message, "body")?,
    };
    protocol::pack(&result).map_err(|_| blinkcms_datarouter::DATAROUTER_ERR_PROTOCOL)?;
    Ok(result)
}

fn java_message<'local>(
    env: &mut JNIEnv<'local>,
    message: &Message,
) -> Result<JObject<'local>, c_int> {
    let var_data = env
        .byte_array_from_slice(&message.var_data)
        .map_err(|_| blinkcms_datarouter::DATAROUTER_ERR_INTERNAL)?;
    let body = env
        .byte_array_from_slice(&message.body)
        .map_err(|_| blinkcms_datarouter::DATAROUTER_ERR_INTERNAL)?;
    let var_data = JObject::from(var_data);
    let body = JObject::from(body);
    env.new_object(
        "io/blinkcms/datarouter/DataRouterMessage",
        "(II[B[B)V",
        &[
            JValue::Int(message.message_type as jint),
            JValue::Int(message.flags as jint),
            JValue::Object(&var_data),
            JValue::Object(&body),
        ],
    )
    .map_err(|_| blinkcms_datarouter::DATAROUTER_ERR_INTERNAL)
}

fn java_received(env: &mut JNIEnv, source: String, message: Message) -> Result<jobject, c_int> {
    let source = env
        .new_string(source)
        .map_err(|_| blinkcms_datarouter::DATAROUTER_ERR_INTERNAL)?;
    let source = JObject::from(source);
    let message = java_message(env, &message)?;
    env.new_object(
        "io/blinkcms/datarouter/ReceivedDataRouterMessage",
        "(Ljava/lang/String;Lio/blinkcms/datarouter/DataRouterMessage;)V",
        &[JValue::Object(&source), JValue::Object(&message)],
    )
    .map(JObject::into_raw)
    .map_err(|_| blinkcms_datarouter::DATAROUTER_ERR_INTERNAL)
}

#[unsafe(no_mangle)]
pub extern "system" fn Java_io_blinkcms_datarouter_DataRouterFrontend_nativeOpen(
    mut env: JNIEnv,
    _class: JClass,
    options: JObject,
) -> jlong {
    run(&mut env, |env| {
        let frontend = Frontend::connect(read_options(env, options)?)?;
        let id = next_handle()?;
        handles()
            .lock()
            .map_err(|_| blinkcms_datarouter::DATAROUTER_ERR_INTERNAL)?
            .insert(id, Arc::new(NativeHandle::new(frontend)));
        Ok(id)
    })
    .unwrap_or(0)
}

#[unsafe(no_mangle)]
pub extern "system" fn Java_io_blinkcms_datarouter_DataRouterFrontend_nativeSend(
    mut env: JNIEnv,
    _class: JClass,
    id: jlong,
    target: JString,
    message: JObject,
    timeout_millis: jlong,
) {
    let _ = run(&mut env, |env| {
        let lease = lease(id)?;
        let target = get_string(env, JObject::from(target))?;
        let message = read_message(env, message)?;
        let packed = protocol::pack(&message).map_err(|_| blinkcms_datarouter::DATAROUTER_ERR_PROTOCOL)?;
        lease.frontend.send(&target, &packed, duration_millis(timeout_millis)?)
    });
}

#[unsafe(no_mangle)]
pub extern "system" fn Java_io_blinkcms_datarouter_DataRouterFrontend_nativeReceive(
    mut env: JNIEnv,
    _class: JClass,
    id: jlong,
    timeout_millis: jlong,
    allow_empty: jboolean,
) -> jobject {
    run(&mut env, |env| {
        let lease = lease(id)?;
        let _receive = lease
            .owner
            .receive_lock
            .lock()
            .map_err(|_| blinkcms_datarouter::DATAROUTER_ERR_INTERNAL)?;
        match lease.frontend.recv(duration_millis(timeout_millis)?) {
            Ok(received) => java_received(env, received.source, received.message),
            Err(code)
                if allow_empty == JNI_TRUE
                    && (code == blinkcms_datarouter::DATAROUTER_ERR_TIMEOUT
                        || code == blinkcms_datarouter::DATAROUTER_ERR_AGAIN) => Ok(std::ptr::null_mut()),
            Err(code) => Err(code),
        }
    })
    .unwrap_or(std::ptr::null_mut())
}

fn java_stats(env: &mut JNIEnv, id: jlong) -> Result<jobject, c_int> {
    let lease = lease(id)?;
    let stats = lease.frontend.stats_snapshot();
    let values = [
        stats.sent_frames, stats.sent_chunks, stats.received_frames, stats.received_complete,
        stats.received_chunk_acks, stats.received_chunk_errors, stats.ack_timeouts,
        stats.queue_full, stats.protocol_errors, stats.dropped_frames, stats.pending_acks,
        stats.queued_messages, stats.reader_alive, stats.reader_errors, stats.keepalive_ticks,
        stats.last_keepalive_ns, stats.reassembly_expired, stats.reassembly_rejected,
        stats.corrupted_rings, stats.registration_recoveries, stats.blocked_wakeups,
        stats.dead_process_reaps, stats.transport_generation, stats.switch_recoveries,
    ];
    let args: Vec<JValue<'_, '_>> = values.into_iter().map(|value| JValue::Long(value as jlong)).collect();
    env.new_object(
        "io/blinkcms/datarouter/DataRouterStats",
        "(JJJJJJJJJJJJJJJJJJJJJJJJ)V",
        &args,
    )
    .map(JObject::into_raw)
    .map_err(|_| blinkcms_datarouter::DATAROUTER_ERR_INTERNAL)
}

#[unsafe(no_mangle)]
pub extern "system" fn Java_io_blinkcms_datarouter_DataRouterFrontend_nativeStats(
    mut env: JNIEnv,
    _class: JClass,
    id: jlong,
) -> jobject {
    run(&mut env, |env| java_stats(env, id)).unwrap_or(std::ptr::null_mut())
}

fn source_name(source: &[i8; blinkcms_datarouter::DATAROUTER_SERVICE_NAME_MAX + 1]) -> String {
    source
        .iter()
        .copied()
        .take_while(|byte| *byte != 0)
        .map(|byte| byte as u8)
        .collect::<Vec<_>>()
        .into_iter()
        .map(char::from)
        .collect()
}

fn java_registry_snapshot(env: &mut JNIEnv, id: jlong) -> Result<jobject, c_int> {
    let lease = lease(id)?;
    let mut entries = vec![
        blinkcms_datarouter::datarouter_registry_snapshot_entry {
            service_name: [0; blinkcms_datarouter::DATAROUTER_SERVICE_NAME_MAX + 1],
            pid: 0,
            route_id: 0,
            channel_id: 0,
            slot: 0,
            last_seen_ns: 0,
        };
        blinkcms_datarouter::DATAROUTER_MAX_PORTS
    ];
    let mut count = 0usize;
    let code = lease.frontend.registry_snapshot(&mut entries, &mut count);
    if code != blinkcms_datarouter::DATAROUTER_OK && code != blinkcms_datarouter::DATAROUTER_ERR_FULL {
        return Err(code);
    }
    let list = env
        .new_object("java/util/ArrayList", "()V", &[])
        .map_err(|_| blinkcms_datarouter::DATAROUTER_ERR_INTERNAL)?;
    for entry in entries.into_iter().take(count) {
        let name = env
            .new_string(source_name(&entry.service_name))
            .map_err(|_| blinkcms_datarouter::DATAROUTER_ERR_INTERNAL)?;
        let name = JObject::from(name);
        let item = env
            .new_object(
                "io/blinkcms/datarouter/RegistryEntry",
                "(Ljava/lang/String;IJJJJ)V",
                &[
                    JValue::Object(&name),
                    JValue::Int(entry.pid),
                    JValue::Long(entry.route_id as jlong),
                    JValue::Long(entry.channel_id as jlong),
                    JValue::Long(entry.slot as jlong),
                    JValue::Long(entry.last_seen_ns as jlong),
                ],
            )
            .map_err(|_| blinkcms_datarouter::DATAROUTER_ERR_INTERNAL)?;
        env.call_method(&list, "add", "(Ljava/lang/Object;)Z", &[JValue::Object(&item)])
            .map_err(|_| blinkcms_datarouter::DATAROUTER_ERR_INTERNAL)?;
    }
    env.new_object(
        "io/blinkcms/datarouter/RegistrySnapshot",
        "(Ljava/util/List;)V",
        &[JValue::Object(&list)],
    )
    .map(JObject::into_raw)
    .map_err(|_| blinkcms_datarouter::DATAROUTER_ERR_INTERNAL)
}

#[unsafe(no_mangle)]
pub extern "system" fn Java_io_blinkcms_datarouter_DataRouterFrontend_nativeRegistrySnapshot(
    mut env: JNIEnv,
    _class: JClass,
    id: jlong,
) -> jobject {
    run(&mut env, |env| java_registry_snapshot(env, id)).unwrap_or(std::ptr::null_mut())
}

#[unsafe(no_mangle)]
pub extern "system" fn Java_io_blinkcms_datarouter_DataRouterFrontend_nativeCompatibility(
    mut env: JNIEnv,
    _class: JClass,
) -> jobject {
    run(&mut env, |env| {
        let version = env
            .new_string(blinkcms_datarouter::version().to_string_lossy())
            .map_err(|_| blinkcms_datarouter::DATAROUTER_ERR_INTERNAL)?;
        let fingerprint = env
            .new_string(ABI_FINGERPRINT)
            .map_err(|_| blinkcms_datarouter::DATAROUTER_ERR_INTERNAL)?;
        let version = JObject::from(version);
        let fingerprint = JObject::from(fingerprint);
        env.new_object(
            "io/blinkcms/datarouter/DataRouterCompatibility",
            "(Ljava/lang/String;IIIILjava/lang/String;)V",
            &[
                JValue::Object(&version),
                JValue::Int(LAYOUT_VERSION),
                JValue::Int(FRONTEND_ABI_VERSION),
                JValue::Int(JNI_ABI_VERSION),
                JValue::Int(blinkcms_datarouter::DATAROUTER_CHANNEL_SIZE as jint),
                JValue::Object(&fingerprint),
            ],
        )
        .map(JObject::into_raw)
        .map_err(|_| blinkcms_datarouter::DATAROUTER_ERR_INTERNAL)
    })
    .unwrap_or(std::ptr::null_mut())
}

#[unsafe(no_mangle)]
pub extern "system" fn Java_io_blinkcms_datarouter_DataRouterFrontend_nativeClose(
    mut env: JNIEnv,
    _class: JClass,
    id: jlong,
) {
    let _ = run(&mut env, |_| {
        if id <= 0 {
            return Err(blinkcms_datarouter::DATAROUTER_ERR_INVALID_ARGUMENT);
        }
        let handle = handles()
            .lock()
            .map_err(|_| blinkcms_datarouter::DATAROUTER_ERR_INTERNAL)?
            .remove(&id)
            .ok_or(blinkcms_datarouter::DATAROUTER_ERR_NOT_FOUND)?;
        handle.close();
        Ok(())
    });
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn compatibility_fingerprint_is_fixed_for_jni_abi_one() {
        assert_eq!(FRONTEND_ABI_VERSION, 2);
        assert_eq!(JNI_ABI_VERSION, 1);
        assert_eq!(blinkcms_datarouter::DATAROUTER_CHANNEL_SIZE, 524_288);
        assert_eq!(ABI_FINGERPRINT.len(), 64);
    }
}
