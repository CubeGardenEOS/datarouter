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

use blinkcms_datarouter::frontend::{Frontend, FrontendOptions, RecvPackedError};
use blinkcms_datarouter::protocol;
use jni::objects::{GlobalRef, JByteArray, JClass, JFieldID, JMethodID, JObject, JString, JValue, ReleaseMode};
use jni::signature::{Primitive, ReturnType};
use jni::sys::{jboolean, jfieldID, jint, jlong, jobject, jvalue, jmethodID, JNI_TRUE};
use jni::JNIEnv;
use std::cell::RefCell;

const JNI_ABI_VERSION: jint = 1;
const LAYOUT_VERSION: jint = 9;
const FRONTEND_ABI_VERSION: jint = 2;
const ABI_FINGERPRINT: &str = "9805e860b0e73cda067a1b8b1b64904ab60d3800d6567308b7dab4e2defe2190";
const JAVA_EXCEPTION: &str = "io/blinkcms/datarouter/DataRouterException";
const MAX_TIMEOUT_MILLIS: jlong = i32::MAX as jlong;

/// Resolved-once JNI field/method IDs and class references for the hot-path
/// message accessors and constructors.
///
/// Two kinds of per-call overhead are eliminated here:
///
/// 1. The `jni` crate's checked `call_method`/`new_object`/`find_class` parse
///    the JVM type signature / look up classes on EVERY call (measured
///    ~34 µs for a signature parse, and `FindClass` on every `new_object`
///    with a string class name). Resolving the IDs and class references
///    once and calling the unchecked variants drops each call to the raw
///    JNI `Get*Field`/`Call*Method`/`NewObject` price.
/// 2. The send path reads `messageType`/`flags`/`varData` straight from
///    their backing fields (2×`GetIntField` + 1×`GetObjectField`) instead
///    of 4 virtual method calls; only the body goes through the
///    `bodyUnsafe()` method because a zero-copy received message
///    materializes its body on first access.
///
/// Field/method IDs and global class refs stay valid as long as the
/// referencing classes are loaded, which is guaranteed for the lifetime of
/// the JNI library.
struct CachedMethods {
    f_message_type: jfieldID,
    f_flags: jfieldID,
    f_var_data: jfieldID,
    m_body_unsafe: jmethodID,
    msg_ctor_nocopy: jmethodID,
    msg_ctor_zerocopy: jmethodID,
    received_ctor: jmethodID,
    msg_class: GlobalRef,
    received_class: GlobalRef,
}
// jfieldIDs/jmethodIDs are plain pointers valid for the JVM's lifetime once
// resolved; the values are never mutated. GlobalRef is Send+Sync (Arc over a
// JVM global ref). Sharing the resolved set across threads is safe.
unsafe impl Send for CachedMethods {}
unsafe impl Sync for CachedMethods {}

static METHODS: OnceLock<Result<CachedMethods, c_int>> = OnceLock::new();

fn methods(env: &mut JNIEnv) -> Result<&'static CachedMethods, c_int> {
    match METHODS.get_or_init(|| init_methods(env)) {
        Ok(methods) => Ok(methods),
        Err(code) => Err(*code),
    }
}

fn init_methods(env: &mut JNIEnv) -> Result<CachedMethods, c_int> {
    let msg_class = env
        .find_class("io/blinkcms/datarouter/DataRouterMessage")
        .map_err(|_| blinkcms_datarouter::DATAROUTER_ERR_INTERNAL)?;
    let received_class = env
        .find_class("io/blinkcms/datarouter/ReceivedDataRouterMessage")
        .map_err(|_| blinkcms_datarouter::DATAROUTER_ERR_INTERNAL)?;
    fn fid(env: &mut JNIEnv, class: &JClass, name: &str, sig: &str) -> Result<jfieldID, c_int> {
        env.get_field_id(class, name, sig)
            .map(|field| field.into_raw())
            .map_err(|_| blinkcms_datarouter::DATAROUTER_ERR_INTERNAL)
    }
    fn mid(env: &mut JNIEnv, class: &JClass, name: &str, sig: &str) -> Result<jmethodID, c_int> {
        env.get_method_id(class, name, sig)
            .map(|method| method.into_raw())
            .map_err(|_| blinkcms_datarouter::DATAROUTER_ERR_INTERNAL)
    }
    let msg_class_ref = env
        .new_global_ref(&msg_class)
        .map_err(|_| blinkcms_datarouter::DATAROUTER_ERR_INTERNAL)?;
    let received_class_ref = env
        .new_global_ref(&received_class)
        .map_err(|_| blinkcms_datarouter::DATAROUTER_ERR_INTERNAL)?;
    Ok(CachedMethods {
        f_message_type: fid(env, &msg_class, "messageType", "I")?,
        f_flags: fid(env, &msg_class, "flags", "I")?,
        f_var_data: fid(env, &msg_class, "varData", "[B")?,
        m_body_unsafe: mid(env, &msg_class, "bodyUnsafe", "()[B")?,
        msg_ctor_nocopy: mid(env, &msg_class, "<init>", "(II[B[BZ)V")?,
        msg_ctor_zerocopy: mid(
            env,
            &msg_class,
            "<init>",
            "(II[BLjava/nio/ByteBuffer;)V",
        )?,
        received_ctor: mid(
            env,
            &received_class,
            "<init>",
            "(Ljava/lang/String;Lio/blinkcms/datarouter/DataRouterMessage;)V",
        )?,
        msg_class: msg_class_ref,
        received_class: received_class_ref,
    })
}

fn mid(raw: jmethodID) -> Result<JMethodID, c_int> {
    if raw.is_null() {
        return Err(blinkcms_datarouter::DATAROUTER_ERR_INTERNAL);
    }
    Ok(unsafe { JMethodID::from_raw(raw) })
}

fn call_bytes<'local>(
    env: &mut JNIEnv<'local>,
    object: &JObject,
    raw: jmethodID,
) -> Result<JObject<'local>, c_int> {
    unsafe {
        env.call_method_unchecked(
            object,
            mid(raw)?,
            ReturnType::Array,
            &[],
        )
    }
    .map_err(|_| blinkcms_datarouter::DATAROUTER_ERR_INVALID_ARGUMENT)?
    .l()
    .map_err(|_| blinkcms_datarouter::DATAROUTER_ERR_INVALID_ARGUMENT)
}

fn get_int_field(env: &mut JNIEnv, object: &JObject, raw: jfieldID) -> Result<jint, c_int> {
    if raw.is_null() {
        return Err(blinkcms_datarouter::DATAROUTER_ERR_INTERNAL);
    }
    unsafe { env.get_field_unchecked(object, JFieldID::from_raw(raw), ReturnType::Primitive(Primitive::Int)) }
        .map_err(|_| blinkcms_datarouter::DATAROUTER_ERR_INVALID_ARGUMENT)?
        .i()
        .map_err(|_| blinkcms_datarouter::DATAROUTER_ERR_INVALID_ARGUMENT)
}

fn get_obj_field<'local>(
    env: &mut JNIEnv<'local>,
    object: &JObject,
    raw: jfieldID,
) -> Result<JObject<'local>, c_int> {
    if raw.is_null() {
        return Err(blinkcms_datarouter::DATAROUTER_ERR_INTERNAL);
    }
    unsafe { env.get_field_unchecked(object, JFieldID::from_raw(raw), ReturnType::Array) }
        .map_err(|_| blinkcms_datarouter::DATAROUTER_ERR_INVALID_ARGUMENT)?
        .l()
        .map_err(|_| blinkcms_datarouter::DATAROUTER_ERR_INVALID_ARGUMENT)
}

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
    // get_string_unchecked skips the checked variant's per-call
    // find_class("java/lang/String") + is_assignable_from lookup; the
    // caller already hands us a real java.lang.String.
    let string = JString::from(object);
    unsafe { env.get_string_unchecked(&string) }
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
        auth_token: None, // JNI ABI v1 predates switch auth; --token switches reject here.
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

// Reusable wire-format buffers for the send/recv hot paths. `pack_into` /
// `recv_packed` clear/overwrite but retain the allocation, so large-payload
// transfers stop paying for a fresh 300 KiB mmap'd buffer (with per-page
// faults) on every call. The transport operations are synchronous (data is
// copied into / out of the RX ring before they return), so reusing the
// buffers across calls is safe. `thread_local!` because calls can come from
// multiple Java threads.
thread_local! {
    static SEND_BUF: std::cell::RefCell<Vec<u8>> = const { std::cell::RefCell::new(Vec::new()) };
    static RECV_BUF: std::cell::RefCell<Vec<u8>> = const { std::cell::RefCell::new(Vec::new()) };
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
        let methods = methods(env)?;
        let message_type = get_int_field(env, &message, methods.f_message_type)? as u8;
        let flags = get_int_field(env, &message, methods.f_flags)? as u8;
        let vd_object = get_obj_field(env, &message, methods.f_var_data)?;
        // body goes through the bodyUnsafe() method rather than a raw field
        // read: a zero-copy received message keeps its body in a direct
        // ByteBuffer and materializes it on first access.
        let body_object = call_bytes(env, &message, methods.m_body_unsafe)?;
        let vd_array = JByteArray::from(vd_object);
        let body_array = JByteArray::from(body_object);
        let vd_len = env
            .get_array_length(&vd_array)
            .map_err(|_| blinkcms_datarouter::DATAROUTER_ERR_INVALID_ARGUMENT)? as usize;
        let body_len = env
            .get_array_length(&body_array)
            .map_err(|_| blinkcms_datarouter::DATAROUTER_ERR_INVALID_ARGUMENT)? as usize;
        let total = 8usize
            .checked_add(vd_len)
            .and_then(|len| len.checked_add(body_len))
            .ok_or(blinkcms_datarouter::DATAROUTER_ERR_PROTOCOL)?;
        if vd_len > u16::MAX as usize
            || body_len > u32::MAX as usize
            || total > protocol::MAX_APP_PAYLOAD
        {
            return Err(blinkcms_datarouter::DATAROUTER_ERR_PAYLOAD_TOO_LARGE);
        }
        SEND_BUF.with(|cell| {
            let mut buf = cell.borrow_mut();
            buf.clear();
            buf.reserve(total);
            buf.push(message_type);
            buf.push(flags);
            buf.extend_from_slice(&(vd_len as u16).to_le_bytes());
            buf.extend_from_slice(&(body_len as u32).to_le_bytes());
            {
                // Critical section: pin the Java var_data array and copy it
                // straight into the wire buffer (no intermediate Vec). No
                // other JNI calls may run while the array is pinned.
                let pinned = unsafe {
                    env.get_array_elements_critical(&vd_array, ReleaseMode::NoCopyBack)
                }
                .map_err(|_| blinkcms_datarouter::DATAROUTER_ERR_INTERNAL)?;
                // jbyte is i8; reinterpret the pinned buffer as u8 (zero-cost).
                let var_data = unsafe { std::slice::from_raw_parts(pinned.as_ptr().cast::<u8>(), vd_len) };
                protocol::validate_tlvs(var_data)
                    .map_err(|_| blinkcms_datarouter::DATAROUTER_ERR_PROTOCOL)?;
                buf.extend_from_slice(var_data);
            }
            {
                let pinned = unsafe {
                    env.get_array_elements_critical(&body_array, ReleaseMode::NoCopyBack)
                }
                .map_err(|_| blinkcms_datarouter::DATAROUTER_ERR_INTERNAL)?;
                let body = unsafe { std::slice::from_raw_parts(pinned.as_ptr().cast::<u8>(), body_len) };
                buf.extend_from_slice(body);
            }
            lease.frontend.send(&target, &buf, duration_millis(timeout_millis)?)
        })
    });
}

// Per-thread cache of the last seen source service name as a JVM String.
// Repeatedly receiving from the same peer (the overwhelmingly common
// single-peer case) then reuses the identical java.lang.String object and
// skips both the Rust-side name conversion and a JVM string allocation +
// UTF-8→UTF-16 transcode on every message.
thread_local! {
    static SOURCE_CACHE: RefCell<Option<(Vec<u8>, GlobalRef)>> = const { RefCell::new(None) };
}

/// Return a JVM `String` for `info.source`, reusing the cached object when
/// the name matches the previous receive on this thread.
fn source_string(
    env: &mut JNIEnv,
    source: &[i8; blinkcms_datarouter::DATAROUTER_SERVICE_NAME_MAX + 1],
) -> Result<jobject, c_int> {
    let bytes: Vec<u8> = source
        .iter()
        .copied()
        .take_while(|byte| *byte != 0)
        .map(|byte| byte as u8)
        .collect();
    if let Some(cached) = SOURCE_CACHE.with(|cell| cell.borrow().clone()) {
        if cached.0 == bytes {
            return Ok(cached.1.as_obj().as_raw());
        }
    }
    let java = env
        .new_string(String::from_utf8_lossy(&bytes))
        .map_err(|_| blinkcms_datarouter::DATAROUTER_ERR_INTERNAL)?;
    let raw = java.as_raw();
    let global = env
        .new_global_ref(&java)
        .map_err(|_| blinkcms_datarouter::DATAROUTER_ERR_INTERNAL)?;
    SOURCE_CACHE.with(|cell| *cell.borrow_mut() = Some((bytes, global)));
    Ok(raw)
}

fn receive_impl(
    env: &mut JNIEnv,
    id: jlong,
    timeout_millis: jlong,
    allow_empty: jboolean,
    zero_copy: bool,
) -> Result<jobject, c_int> {
    let lease = lease(id)?;
    let _receive = lease
        .owner
        .receive_lock
        .lock()
        .map_err(|_| blinkcms_datarouter::DATAROUTER_ERR_INTERNAL)?;
    let timeout_ms = duration_millis(timeout_millis)?;
    let methods = methods(env)?;
    RECV_BUF.with(|cell| {
        let mut buf = cell.borrow_mut();
        // recv_packed copies the RX frame straight into `buf` (no
        // per-message Vec materialization / protocol::unpack); a short
        // buffer reports the required size and keeps the frame for retry.
        let info = loop {
            match lease.frontend.recv_packed(&mut buf, timeout_ms) {
                Ok(info) => break info,
                Err(RecvPackedError::BufferTooSmall { required }) => {
                    buf.resize(required, 0);
                }
                Err(RecvPackedError::Code(code))
                    if allow_empty == JNI_TRUE
                        && (code == blinkcms_datarouter::DATAROUTER_ERR_TIMEOUT
                            || code == blinkcms_datarouter::DATAROUTER_ERR_AGAIN) =>
                {
                    return Ok(std::ptr::null_mut());
                }
                Err(RecvPackedError::Code(code)) => return Err(code),
            }
        };
        let len = info.len;
        if len < protocol::HEADER_SIZE || len > buf.len() {
            return Err(blinkcms_datarouter::DATAROUTER_ERR_PROTOCOL);
        }
        let packed = &buf[..len];
        let message_type = packed[0];
        let flags = packed[1];
        let vd_len = u16::from_le_bytes([packed[2], packed[3]]) as usize;
        let b_len = u32::from_le_bytes([packed[4], packed[5], packed[6], packed[7]]) as usize;
        if protocol::HEADER_SIZE + vd_len + b_len != len {
            return Err(blinkcms_datarouter::DATAROUTER_ERR_PROTOCOL);
        }
        let source = source_string(env, &info.source)?;
        let var_data = env
            .byte_array_from_slice(
                &packed[protocol::HEADER_SIZE..protocol::HEADER_SIZE + vd_len],
            )
            .map_err(|_| blinkcms_datarouter::DATAROUTER_ERR_INTERNAL)?;
        let var_data = JObject::from(var_data);
        let message = if zero_copy {
            // Zero-copy body: wrap the tail of the reusable receive buffer in
            // a DirectByteBuffer. The returned message materializes a byte[]
            // on first body access; the direct buffer is only valid until
            // the next receive on this thread (the same contract as C#'s
            // preallocated receive buffer). No JVM heap array is allocated
            // for the payload, so large transfers stop paying the JVM's
            // array zeroing + copy on every receive.
            let body_ptr = unsafe { buf.as_ptr().add(protocol::HEADER_SIZE + vd_len) } as *mut u8;
            let body_direct = unsafe { env.new_direct_byte_buffer(body_ptr, b_len) }
                .map_err(|_| blinkcms_datarouter::DATAROUTER_ERR_INTERNAL)?;
            let body_direct = JObject::from(body_direct);
            let args = [
                jvalue { i: message_type as jint },
                jvalue { i: flags as jint },
                jvalue { l: var_data.as_raw() },
                jvalue { l: body_direct.as_raw() },
            ];
            unsafe {
                env.new_object_unchecked(
                    &methods.msg_class,
                    mid(methods.msg_ctor_zerocopy)?,
                    &args,
                )
            }
            .map_err(|_| blinkcms_datarouter::DATAROUTER_ERR_INTERNAL)?
        } else {
            let body = env
                .byte_array_from_slice(&packed[protocol::HEADER_SIZE + vd_len..])
                .map_err(|_| blinkcms_datarouter::DATAROUTER_ERR_INTERNAL)?;
            let body = JObject::from(body);
            let args = [
                jvalue { i: message_type as jint },
                jvalue { i: flags as jint },
                jvalue { l: var_data.as_raw() },
                jvalue { l: body.as_raw() },
                jvalue { z: JNI_TRUE },
            ];
            unsafe {
                env.new_object_unchecked(
                    &methods.msg_class,
                    mid(methods.msg_ctor_nocopy)?,
                    &args,
                )
            }
            .map_err(|_| blinkcms_datarouter::DATAROUTER_ERR_INTERNAL)?
        };
        let args = [
            jvalue { l: source },
            jvalue { l: message.as_raw() },
        ];
        unsafe {
            env.new_object_unchecked(
                &methods.received_class,
                mid(methods.received_ctor)?,
                &args,
            )
        }
        .map(JObject::into_raw)
        .map_err(|_| blinkcms_datarouter::DATAROUTER_ERR_INTERNAL)
    })
}

#[unsafe(no_mangle)]
pub extern "system" fn Java_io_blinkcms_datarouter_DataRouterFrontend_nativeReceive(
    mut env: JNIEnv,
    _class: JClass,
    id: jlong,
    timeout_millis: jlong,
    allow_empty: jboolean,
) -> jobject {
    run(&mut env, |env| receive_impl(env, id, timeout_millis, allow_empty, false))
        .unwrap_or(std::ptr::null_mut())
}

/// Zero-copy receive: like [`Java_io_blinkcms_datarouter_DataRouterFrontend_nativeReceive`]
/// but the message body is a DirectByteBuffer view over the reusable receive
/// buffer (valid until the next receive on the same thread; materialized into
/// a byte[] on first access). See `DataRouterFrontend#receiveZeroCopy`.
#[unsafe(no_mangle)]
pub extern "system" fn Java_io_blinkcms_datarouter_DataRouterFrontend_nativeReceiveZeroCopy(
    mut env: JNIEnv,
    _class: JClass,
    id: jlong,
    timeout_millis: jlong,
    allow_empty: jboolean,
) -> jobject {
    run(&mut env, |env| receive_impl(env, id, timeout_millis, allow_empty, true))
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
        assert_eq!(blinkcms_datarouter::DATAROUTER_RING_CAPACITY, 524_288);
        assert_eq!(
            blinkcms_datarouter::DATAROUTER_MAX_APP_PAYLOAD,
            524_288 - 8
        );
        assert_eq!(blinkcms_datarouter::DATAROUTER_RING_HEADER_SIZE, 8);
        assert_eq!(blinkcms_datarouter::DATAROUTER_FRAME_HEADER_SIZE, 8);
        assert_eq!(ABI_FINGERPRINT.len(), 64);
    }
}
