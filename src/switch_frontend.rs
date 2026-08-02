//! Embeddable switch frontend: lifecycle, capacity, and auth for the
//! `datarouter-switch` process, usable as a Rust package from other projects.
//!
//! # Lifecycle
//!
//! [`SwitchHandle::start`] spawns the `datarouter-switch` binary as a child
//! process. The binary is **embedded in the importing executable** at compile
//! time (`include_bytes!` in `build.rs`); at runtime it is executed from an
//! anonymous Linux `memfd` (zero files on disk) or a transient 0700 temp file
//! that is unlinked as soon as the switch is up. No scripts, config files, or
//! directory resources are required — a consuming project only calls
//! [`SwitchHandle::start`]. [`stop`] and [`restart`] manage the process; on
//! graceful exit the switch unlinks its shared-memory object, and any
//! connected [`crate::frontend::Frontend`] clients automatically re-register
//! on the next touch/send (the transport already treats a missing self slot
//! as a recovery event).
//!
//! # Dynamic capacity
//!
//! The switch starts with `max_ports` slots (default 4, up to 64). When the
//! transport is nearly full, [`expand_capacity`] gracefully restarts the
//! switch with a larger slot count. Existing frontends survive the restart by
//! reconnecting; [`ensure_capacity`] snapshots the running switch and expands
//! only when needed.
//!
//! # Simple auth
//!
//! Starting the switch with [`SwitchConfig::token`] stores only an FNV-1a
//! fingerprint in shared memory. `datarouter_open` rejects any client whose
//! `auth_token` does not match (`DATAROUTER_ERR_PERMISSION`), so the raw
//! secret never crosses process boundaries after startup. [`verify_auth`]
//! probes that an unauthenticated open is refused and an authenticated open
//! succeeds.
//!
//! [`stop`]: SwitchHandle::stop
//! [`restart`]: SwitchHandle::restart
//! [`expand_capacity`]: SwitchHandle::expand_capacity
//! [`ensure_capacity`]: SwitchHandle::ensure_capacity
//! [`verify_auth`]: SwitchHandle::verify_auth

use std::ffi::CString;
use std::io;
use std::path::{Path, PathBuf};
use std::process::{Child, Command, Stdio};
use std::time::{Duration, Instant};

use crate::frontend::port_frontend::PortFrontend;

/// Default number of registerable processes when the frontend starts a switch.
pub const DEFAULT_MAX_PORTS: u32 = crate::DATAROUTER_DEFAULT_MAX_PORTS as u32;
/// Hard upper bound for registerable processes (control-plane array size).
pub const HARD_MAX_PORTS: u32 = crate::DATAROUTER_MAX_PORTS as u32;

/// Switch log verbosity, mirroring `--log-level`.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum LogLevel {
    Error,
    Warn,
    #[default]
    Info,
    Debug,
    Trace,
}

impl LogLevel {
    fn as_str(self) -> &'static str {
        match self {
            LogLevel::Error => "error",
            LogLevel::Warn => "warn",
            LogLevel::Info => "info",
            LogLevel::Debug => "debug",
            LogLevel::Trace => "trace",
        }
    }
}

/// Configuration for starting one `datarouter-switch` process.
#[derive(Debug, Clone)]
pub struct SwitchConfig {
    /// Shared-memory object name, e.g. `/datarouter`.
    pub shm_name: String,
    /// Shared-memory mode (octal, e.g. `0o600`).
    pub mode: u32,
    /// Registration TTL in milliseconds.
    pub ttl_ms: u32,
    /// Initial number of registerable slots (1..=64). Growth is possible at
    /// runtime via [`SwitchHandle::expand_capacity`].
    pub max_ports: u32,
    /// Allowlisted service names (empty means nothing may register unless
    /// [`allow_any`](SwitchConfig::allow_any) is set).
    pub allow: Vec<String>,
    /// Permit any service name to register (explicit opt-in, mirrors
    /// `--allow-any`). Conflicts with a non-empty [`allow`](SwitchConfig::allow).
    pub allow_any: bool,
    /// Optional shared secret enabling simple auth.
    pub token: Option<String>,
    /// Reap registrations whose owning process is confirmed dead.
    pub reap_dead_processes: bool,
    /// Switch log level.
    pub log_level: LogLevel,
    /// Explicit path to the `datarouter-switch` binary. When `None`, the
    /// binary built by this crate (or `$DATAROUTER_SWITCH_BIN`) is used.
    pub binary: Option<PathBuf>,
    /// Where to redirect switch stdout/stderr. `None` inherits the parent.
    pub log_file: Option<PathBuf>,
    /// How long [`SwitchHandle::start`] waits for the transport to come up.
    pub ready_timeout: Duration,
}

impl Default for SwitchConfig {
    fn default() -> Self {
        Self {
            shm_name: String::from_utf8_lossy(
                &crate::DATAROUTER_DEFAULT_SHM_NAME[..crate::DATAROUTER_DEFAULT_SHM_NAME.len() - 1],
            )
            .into_owned(),
            mode: 0o600,
            ttl_ms: crate::DATAROUTER_DEFAULT_TIMEOUT_MS,
            max_ports: DEFAULT_MAX_PORTS,
            allow: Vec::new(),
            allow_any: false,
            token: None,
            reap_dead_processes: false,
            log_level: LogLevel::Info,
            binary: None,
            log_file: None,
            ready_timeout: Duration::from_secs(5),
        }
    }
}

impl SwitchConfig {
    /// Shortcut for tests and tools: `/datarouter-{name}`, one allowed
    /// service, default capacity.
    pub fn simple(name: &str, service: &str) -> Self {
        Self {
            shm_name: format!("/datarouter-{name}"),
            allow: vec![service.to_owned()],
            ..Self::default()
        }
    }

    /// Validate the configuration; mirrors the C switch's argument rules.
    pub fn validate(&self) -> Result<(), SwitchError> {
        if !(1..=HARD_MAX_PORTS).contains(&self.max_ports) {
            return Err(SwitchError::InvalidConfig(format!(
                "max_ports {} out of range 1..={HARD_MAX_PORTS}",
                self.max_ports
            )));
        }
        if !self.allow_any && self.allow.is_empty() {
            return Err(SwitchError::InvalidConfig(
                "an allowlist is required; set allow or allow_any".to_owned(),
            ));
        }
        if self.allow_any && !self.allow.is_empty() {
            return Err(SwitchError::InvalidConfig(
                "allow_any conflicts with an explicit allowlist".to_owned(),
            ));
        }
        if self.allow.len() as u32 > self.max_ports {
            return Err(SwitchError::InvalidConfig(format!(
                "allowlist needs {} ports but max_ports is {}",
                self.allow.len(),
                self.max_ports
            )));
        }
        Ok(())
    }

    /// Build the argument vector for the switch process.
    fn args(&self) -> Vec<String> {
        let mut args = vec![
            "--name".to_owned(),
            self.shm_name.clone(),
            "--mode".to_owned(),
            format!("{:04o}", self.mode),
            "--ttl-ms".to_owned(),
            self.ttl_ms.to_string(),
            "--max-ports".to_owned(),
            self.max_ports.to_string(),
        ];
        if !self.allow.is_empty() {
            args.push("--allow".to_owned());
            args.push(self.allow.join(","));
        }
        if self.allow_any {
            args.push("--allow-any".to_owned());
        }
        if let Some(token) = &self.token {
            args.push("--token".to_owned());
            args.push(token.clone());
        }
        if self.reap_dead_processes {
            args.push("--reap-dead-processes".to_owned());
        }
        args.push("--log-level".to_owned());
        args.push(self.log_level.as_str().to_owned());
        args
    }

    fn command(&self, program: &Path) -> Result<Command, SwitchError> {
        let mut cmd = Command::new(program);
        cmd.args(self.args());
        match &self.log_file {
            Some(path) => {
                let file = std::fs::OpenOptions::new()
                    .create(true)
                    .append(true)
                    .open(path)
                    .map_err(SwitchError::Io)?;
                cmd.stdout(Stdio::from(file.try_clone().map_err(SwitchError::Io)?))
                    .stderr(Stdio::from(file));
            }
            None => {
                cmd.stdout(Stdio::null()).stderr(Stdio::null());
            }
        }
        Ok(cmd)
    }
}

/// A materialized switch executable. The switch binary is embedded in the
/// consuming executable via `include_bytes!`; at runtime it is made executable
/// from memory (Linux `memfd_create`) or a transient temp file, so the switch
/// never depends on resource files in any directory. Explicit `binary` paths
/// and `$DATAROUTER_SWITCH_BIN` are used in place, never deleted.
struct Materialized {
    path: PathBuf,
    owned: bool,
    memfd: Option<std::os::fd::RawFd>,
}

impl Materialized {
    fn external(path: PathBuf) -> Self {
        Self {
            path,
            owned: false,
            memfd: None,
        }
    }

    fn program(&self) -> &Path {
        &self.path
    }
}

impl Drop for Materialized {
    fn drop(&mut self) {
        if self.owned {
            if let Some(fd) = self.memfd.take() {
                // SAFETY: our own memfd, closed exactly once.
                unsafe {
                    libc::close(fd);
                }
            } else {
                let _ = std::fs::remove_file(&self.path);
            }
        }
    }
}

fn materialize_switch(config: &SwitchConfig) -> Result<Materialized, SwitchError> {
    if let Some(binary) = &config.binary {
        if binary.is_file() {
            return Ok(Materialized::external(binary.clone()));
        }
        return Err(SwitchError::BinaryNotFound(binary.clone()));
    }
    if let Ok(binary) = std::env::var("DATAROUTER_SWITCH_BIN")
        && !binary.is_empty()
        && Path::new(&binary).is_file()
    {
        return Ok(Materialized::external(PathBuf::from(binary)));
    }
    if !crate::DATAROUTER_SWITCH_BYTES.is_empty() {
        #[cfg(target_os = "linux")]
        if let Some(path) = memfd_executable(&crate::DATAROUTER_SWITCH_BYTES) {
            return Ok(path);
        }
        return temp_file_executable(&crate::DATAROUTER_SWITCH_BYTES);
    }
    let legacy = PathBuf::from("build/datarouter-switch");
    if legacy.is_file() {
        return Ok(Materialized::external(legacy));
    }
    Err(SwitchError::BinaryNotFound(legacy))
}

/// Linux: execute the embedded bytes straight from an anonymous memfd via
/// `/proc/self/fd/N` (no file on disk). Falls back to a temp file when memfd
/// or `/proc` is unavailable.
#[cfg(target_os = "linux")]
fn memfd_executable(bytes: &[u8]) -> Option<Materialized> {
    // SAFETY: memfd_create with no flags (no CLOEXEC: the child needs the fd
    // to exec itself through /proc/self/fd).
    let fd = unsafe { libc::syscall(libc::SYS_memfd_create, c"datarouter-switch".as_ptr(), 0u32) }
        as libc::c_int;
    if fd < 0 {
        return None;
    }
    let mut written = 0usize;
    while written < bytes.len() {
        // SAFETY: fd is a valid memfd, buffer is the embedded slice.
        let n = unsafe {
            libc::write(
                fd,
                bytes.as_ptr().add(written).cast(),
                bytes.len() - written,
            )
        };
        if n <= 0 {
            // SAFETY: our own memfd.
            unsafe { libc::close(fd) };
            return None;
        }
        written += n as usize;
    }
    // SAFETY: fd is a valid memfd.
    if unsafe { libc::fchmod(fd, 0o700) } != 0 {
        unsafe { libc::close(fd) };
        return None;
    }
    let path = PathBuf::from(format!("/proc/self/fd/{fd}"));
    Some(Materialized {
        path,
        owned: true,
        memfd: Some(fd),
    })
}

/// Portable fallback: write the embedded bytes to a unique 0700 temp file.
/// On Unix the file is unlinked right after the switch is up; on all
/// platforms it is removed on stop/drop.
fn temp_file_executable(bytes: &[u8]) -> Result<Materialized, SwitchError> {
    use std::io::Write;
    let nanos = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .expect("system time")
        .as_nanos();
    let path =
        std::env::temp_dir().join(format!("datarouter-switch-{}-{nanos}", std::process::id()));
    let mut options = std::fs::OpenOptions::new();
    options.write(true).create_new(true);
    let mut file = options.open(&path).map_err(SwitchError::Io)?;
    file.write_all(bytes).map_err(SwitchError::Io)?;
    drop(file);
    // SAFETY: only the uid can read/write/execute the file (0700).
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        let _ = std::fs::set_permissions(&path, std::fs::Permissions::from_mode(0o700));
    }
    Ok(Materialized {
        path,
        owned: true,
        memfd: None,
    })
}

/// Snapshot of a running switch's transport state.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct SwitchStatus {
    pub shm_name: String,
    pub max_ports: u32,
    pub active_ports: u32,
    pub registered: Vec<String>,
}

/// Errors produced by the switch frontend.
#[derive(Debug)]
pub enum SwitchError {
    Io(io::Error),
    BinaryNotFound(PathBuf),
    InvalidConfig(String),
    StartTimedOut(Duration),
    StopTimedOut(Duration),
    AlreadyRunning(String),
    NotRunning(String),
    Probe(crate::c_int),
    /// Capacity changes below the current number of active ports.
    CapacityShrink {
        current: u32,
        requested: u32,
    },
}

impl std::fmt::Display for SwitchError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            SwitchError::Io(err) => write!(f, "switch frontend io error: {err}"),
            SwitchError::BinaryNotFound(path) => {
                write!(
                    f,
                    "datarouter-switch binary not found at {}",
                    path.display()
                )
            }
            SwitchError::InvalidConfig(message) => write!(f, "invalid switch config: {message}"),
            SwitchError::StartTimedOut(timeout) => {
                write!(f, "switch did not become ready within {timeout:?}")
            }
            SwitchError::StopTimedOut(timeout) => {
                write!(f, "switch did not stop within {timeout:?}")
            }
            SwitchError::AlreadyRunning(name) => write!(f, "switch for {name} is already running"),
            SwitchError::NotRunning(name) => write!(f, "no switch is running for {name}"),
            SwitchError::Probe(code) => {
                write!(
                    f,
                    "switch probe failed: {}",
                    crate::strerror(*code).to_string_lossy()
                )
            }
            SwitchError::CapacityShrink { current, requested } => write!(
                f,
                "cannot shrink capacity from {current} to {requested} while it is in use"
            ),
        }
    }
}

impl std::error::Error for SwitchError {}

/// A managed `datarouter-switch` process.
pub struct SwitchHandle {
    config: SwitchConfig,
    child: Option<Child>,
    materialized: Option<Materialized>,
}

impl SwitchHandle {
    /// Start a switch and wait for its transport to become ready. The switch
    /// binary is embedded in this executable and spawned from memory (or a
    /// transient temp file); no script or resource file is required.
    pub fn start(config: SwitchConfig) -> Result<Self, SwitchError> {
        config.validate()?;
        let shm_name = config.shm_name.clone();
        if probe_open(&shm_name, config.token.as_deref()).is_ok() {
            return Err(SwitchError::AlreadyRunning(shm_name));
        }
        let mut handle = Self {
            config,
            child: None,
            materialized: None,
        };
        handle.spawn_child()?;
        let ready = handle.wait_ready();
        if ready.is_ok() {
            #[cfg(unix)]
            handle.release_temp_binary();
        }
        ready?;
        Ok(handle)
    }

    fn spawn_child(&mut self) -> Result<(), SwitchError> {
        let materialized = materialize_switch(&self.config)?;
        let mut cmd = self.config.command(materialized.program())?;
        let child = cmd.spawn().map_err(SwitchError::Io)?;
        self.child = Some(child);
        self.materialized = Some(materialized);
        Ok(())
    }

    /// On Unix the temp materialization can be unlinked as soon as the child
    /// has exec'd (the running process keeps the inode alive). The memfd
    /// variant needs no cleanup at all; external binaries are never touched.
    #[cfg(unix)]
    fn release_temp_binary(&mut self) {
        if let Some(materialized) = self.materialized.as_mut()
            && materialized.owned
            && materialized.memfd.is_none()
        {
            let _ = std::fs::remove_file(&materialized.path);
        }
    }
    /// True when the child is alive and its shared-memory transport answers a
    /// probe with the configured token.
    pub fn is_running(&mut self) -> bool {
        if self
            .child
            .as_mut()
            .is_none_or(|child| child.try_wait().ok().flatten().is_some())
        {
            return false;
        }
        probe_open(&self.config.shm_name, self.config.token.as_deref()).is_ok()
    }
    /// Gracefully stop the switch (SIGTERM on Unix, kill on Windows). The
    /// switch unlinks its shared-memory object on exit; any temp materialized
    /// binary is removed.
    pub fn stop(&mut self) -> Result<(), SwitchError> {
        if let Some(mut child) = self.child.take() {
            stop_child(&mut child);
            let deadline = Instant::now() + Duration::from_secs(5);
            while child.try_wait().ok().flatten().is_none() {
                if Instant::now() >= deadline {
                    let _ = child.kill();
                    let _ = child.wait();
                    self.materialized = None;
                    return Err(SwitchError::StopTimedOut(Duration::from_secs(5)));
                }
                std::thread::sleep(Duration::from_millis(10));
            }
        }
        self.materialized = None;
        Ok(())
    }

    /// Restart the switch with the current configuration.
    pub fn restart(&mut self) -> Result<(), SwitchError> {
        self.stop()?;
        self.spawn_child()?;
        self.wait_ready()?;
        #[cfg(unix)]
        self.release_temp_binary();
        Ok(())
    }

    /// Wait until the transport answers a probe, bounded by the config timeout.
    pub fn wait_ready(&self) -> Result<(), SwitchError> {
        let deadline = Instant::now() + self.config.ready_timeout;
        while !probe_open(&self.config.shm_name, self.config.token.as_deref()).is_ok() {
            if Instant::now() >= deadline {
                return Err(SwitchError::StartTimedOut(self.config.ready_timeout));
            }
            std::thread::sleep(Duration::from_millis(10));
        }
        Ok(())
    }

    /// Snapshot capacity and registered services.
    pub fn snapshot(&self) -> Result<SwitchStatus, SwitchError> {
        let port = self.open_probe_port()?;
        let (max_ports, active_ports) = port.capacity().map_err(SwitchError::Probe)?;
        let mut entries = vec![
            crate::datarouter_registry_snapshot_entry {
                service_name: [0; crate::DATAROUTER_SERVICE_NAME_MAX + 1],
                pid: 0,
                route_id: 0,
                channel_id: 0,
                slot: 0,
                last_seen_ns: 0,
            };
            crate::DATAROUTER_MAX_PORTS
        ];
        let mut count = 0usize;
        let rc = port.registry_snapshot(&mut entries, &mut count);
        let mut registered = Vec::new();
        if rc == crate::DATAROUTER_OK || rc == crate::DATAROUTER_ERR_FULL {
            for entry in entries.iter().take(count.min(entries.len())) {
                let name = crate::frontend::c_source_to_string(&entry.service_name);
                if !name.is_empty() {
                    registered.push(name);
                }
            }
        }
        Ok(SwitchStatus {
            shm_name: self.config.shm_name.clone(),
            max_ports,
            active_ports,
            registered,
        })
    }

    /// Grow (or shrink back) the switch capacity. This is a graceful restart
    /// with a new `--max-ports`; existing frontends re-register automatically.
    pub fn expand_capacity(&mut self, new_max_ports: u32) -> Result<SwitchStatus, SwitchError> {
        if !(1..=HARD_MAX_PORTS).contains(&new_max_ports) {
            return Err(SwitchError::InvalidConfig(format!(
                "max_ports {new_max_ports} out of range 1..={HARD_MAX_PORTS}"
            )));
        }
        let current = self.snapshot()?.max_ports;
        if new_max_ports < current {
            let status = self.snapshot()?;
            if status.active_ports > new_max_ports {
                return Err(SwitchError::CapacityShrink {
                    current,
                    requested: new_max_ports,
                });
            }
        }
        self.config.max_ports = new_max_ports;
        self.restart()?;
        self.snapshot()
    }

    /// Ensure the transport can register at least `needed` processes.
    pub fn ensure_capacity(&mut self, needed: u32) -> Result<SwitchStatus, SwitchError> {
        let status = self.snapshot()?;
        if status.max_ports >= needed {
            return Ok(status);
        }
        self.expand_capacity(needed)
    }

    /// Verify simple auth: with a configured token, an unauthenticated open
    /// must fail with `DATAROUTER_ERR_PERMISSION` and an authenticated open
    /// must succeed. Without a token, an open must succeed.
    pub fn verify_auth(&self) -> Result<(), SwitchError> {
        if let Some(token) = self.config.token.as_deref() {
            match probe_open(&self.config.shm_name, None) {
                Err(crate::DATAROUTER_ERR_PERMISSION) => {}
                Err(code) => return Err(SwitchError::Probe(code)),
                Ok(()) => {
                    return Err(SwitchError::InvalidConfig(
                        "switch is running without auth despite a configured token".to_owned(),
                    ));
                }
            }
            probe_open(&self.config.shm_name, Some(token)).map_err(SwitchError::Probe)?;
            Ok(())
        } else {
            probe_open(&self.config.shm_name, None).map_err(SwitchError::Probe)
        }
    }

    /// Shared-memory name of the managed switch.
    pub fn shm_name(&self) -> &str {
        &self.config.shm_name
    }

    /// The token the switch was started with, if any.
    pub fn token(&self) -> Option<&str> {
        self.config.token.as_deref()
    }

    pub fn config(&self) -> &SwitchConfig {
        &self.config
    }

    fn open_probe_port(&self) -> Result<PortFrontend, SwitchError> {
        let options = crate::frontend::options::FrontendOptions {
            shm_name: self.config.shm_name.clone(),
            service_name: String::new(),
            auth_token: self.config.token.clone(),
            connect_timeout_ms: self.config.ttl_ms.max(1),
            ..crate::frontend::options::FrontendOptions::default()
        };
        PortFrontend::open_raw(&options).map_err(|code| {
            if code == crate::DATAROUTER_ERR_PERMISSION {
                SwitchError::InvalidConfig("switch auth token mismatch".to_owned())
            } else {
                SwitchError::Probe(code)
            }
        })
    }
}

impl Drop for SwitchHandle {
    fn drop(&mut self) {
        let _ = self.stop();
    }
}

fn probe_open(shm_name: &str, token: Option<&str>) -> Result<(), crate::c_int> {
    let Ok(shm_name) = CString::new(shm_name) else {
        return Err(crate::DATAROUTER_ERR_INVALID_ARGUMENT);
    };
    let token = token
        .map(CString::new)
        .transpose()
        .map_err(|_| crate::DATAROUTER_ERR_INVALID_ARGUMENT)?;
    let options = crate::datarouter_options {
        shm_name: shm_name.as_ptr(),
        timeout_ms: 500,
        auth_token: token
            .as_ref()
            .map_or(std::ptr::null(), |token| token.as_ptr()),
    };
    let mut port = std::ptr::null_mut();
    let rc = unsafe { crate::datarouter_open(&options, &mut port) };
    if rc == crate::DATAROUTER_OK {
        unsafe { crate::datarouter_close(port) };
        Ok(())
    } else {
        Err(rc)
    }
}

#[cfg(unix)]
fn stop_child(child: &mut Child) {
    // SAFETY: child.id() is a live PID owned by this process.
    unsafe {
        libc::kill(child.id() as libc::pid_t, libc::SIGTERM);
    }
}

#[cfg(not(unix))]
fn stop_child(child: &mut Child) {
    let _ = child.kill();
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::frontend::Frontend;
    use crate::frontend::options::FrontendOptions;
    use crate::protocol;

    fn unique_name(prefix: &str) -> String {
        let nanos = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .expect("system time")
            .as_nanos();
        format!("/datarouter-{prefix}-{}-{nanos}", std::process::id())
    }

    fn cleanup(name: &str) {
        let _ = std::fs::remove_file(format!("/dev/shm/{}", name.trim_start_matches('/')));
    }

    #[test]
    fn lifecycle_start_stop_restart() {
        let name = unique_name("lifecycle");
        cleanup(&name);
        let mut switch = SwitchHandle::start(SwitchConfig {
            shm_name: name.clone(),
            ttl_ms: 30_000,
            allow: vec!["core".to_owned()],
            ..SwitchConfig::default()
        })
        .expect("start switch");
        assert!(switch.is_running());
        let status = switch.snapshot().expect("snapshot");
        assert_eq!(status.max_ports, DEFAULT_MAX_PORTS);
        switch.restart().expect("restart switch");
        assert!(switch.is_running());
        switch.stop().expect("stop switch");
        assert!(!switch.is_running());
        cleanup(&name);
    }

    #[test]
    fn capacity_expands_and_registers_more_processes() {
        let name = unique_name("capacity");
        cleanup(&name);
        let mut switch = SwitchHandle::start(SwitchConfig {
            shm_name: name.clone(),
            ttl_ms: 30_000,
            max_ports: 2,
            allow_any: true,
            ..SwitchConfig::default()
        })
        .expect("start switch");
        let status = switch.snapshot().expect("snapshot");
        assert_eq!(status.max_ports, 2);

        let options = |service: &str| FrontendOptions {
            shm_name: name.clone(),
            service_name: service.to_owned(),
            connect_timeout_ms: 1000,
            recv_timeout_ms: 1000,
            reader_wait_ms: 10,
            ..FrontendOptions::default()
        };
        let _a = Frontend::connect(options("a")).expect("connect a");
        let _b = Frontend::connect(options("b")).expect("connect b");
        assert!(matches!(
            Frontend::connect(options("c")),
            Err(crate::DATAROUTER_ERR_FULL)
        ));

        switch.expand_capacity(4).expect("expand capacity to 4");
        assert_eq!(switch.snapshot().expect("snapshot").max_ports, 4);

        // A new registration now fits after the graceful capacity expansion.
        let _c = Frontend::connect(options("c")).expect("connect c after expansion");
        switch.stop().expect("stop switch");
        cleanup(&name);
    }

    #[test]
    fn auth_token_blocks_unauthenticated_opens() {
        let name = unique_name("auth");
        cleanup(&name);
        let mut switch = SwitchHandle::start(SwitchConfig {
            shm_name: name.clone(),
            ttl_ms: 30_000,
            allow: vec!["core".to_owned()],
            token: Some("hunter2".to_owned()),
            ..SwitchConfig::default()
        })
        .expect("start authenticated switch");
        switch.verify_auth().expect("auth verified");

        let missing = FrontendOptions {
            shm_name: name.clone(),
            service_name: "core".to_owned(),
            connect_timeout_ms: 1000,
            ..FrontendOptions::default()
        };
        assert_eq!(
            Frontend::connect(missing)
                .err()
                .expect("must reject missing token"),
            crate::DATAROUTER_ERR_PERMISSION
        );
        let with_token = FrontendOptions {
            shm_name: name.clone(),
            service_name: "core".to_owned(),
            auth_token: Some("hunter2".to_owned()),
            connect_timeout_ms: 1000,
            recv_timeout_ms: 1000,
            reader_wait_ms: 10,
            ..FrontendOptions::default()
        };
        let _core = Frontend::connect(with_token).expect("token opens the switch");
        switch.stop().expect("stop switch");
        cleanup(&name);
    }

    #[test]
    fn single_frame_512k_message_and_four_copy_path() {
        let name = unique_name("bigframe");
        cleanup(&name);
        let mut switch = SwitchHandle::start(SwitchConfig {
            shm_name: name.clone(),
            ttl_ms: 30_000,
            allow: vec!["core".to_owned(), "portal".to_owned()],
            ..SwitchConfig::default()
        })
        .expect("start switch");
        let options = |service: &str| FrontendOptions {
            shm_name: name.clone(),
            service_name: service.to_owned(),
            connect_timeout_ms: 1000,
            recv_timeout_ms: 2000,
            reader_wait_ms: 10,
            ..FrontendOptions::default()
        };
        let core = Frontend::connect(options("core")).expect("connect core");
        let portal = Frontend::connect(options("portal")).expect("connect portal");

        // ~300 KiB: above the old 60 KiB cap, below the 512 KiB single-frame
        // limit. Must arrive as one frame (no chunking, no ack handshake).
        let body_len = 300 * 1024;
        let payload = vec![0xabu8; body_len];
        let mut msg = protocol::Message::new(protocol::MSG_BUSINESS_REQUEST);
        msg.body = payload;
        let packed = protocol::pack(&msg).expect("pack 300KiB message");
        assert!(
            packed.len() <= crate::DATAROUTER_MAX_APP_PAYLOAD,
            "single frame must fit the 512 KiB ring"
        );
        portal
            .send("core", &packed, 1000)
            .expect("send 300KiB frame");

        let received = core.recv(2000).expect("core receives 300KiB frame");
        assert_eq!(received.source, "portal");
        assert_eq!(received.message.body.len(), body_len);
        assert_eq!(received.message.body[..64], vec![0xabu8; 64]);
        assert_eq!(received.message.body[body_len - 64..], vec![0xabu8; 64]);

        // recv_packed must return the exact raw wire bytes for a single-frame
        // message (the reader queues the ring bytes verbatim; no unpack/repack).
        let mut reply = protocol::Message::new(protocol::MSG_BUSINESS_RESPONSE);
        reply.body = vec![0x21u8; 4096];
        let reply_packed = protocol::pack(&reply).expect("pack reply");
        core.send("portal", &reply_packed, 1000)
            .expect("core replies");
        let mut buf = vec![0u8; crate::DATAROUTER_MAX_APP_PAYLOAD];
        let info = portal
            .recv_packed(&mut buf, 2000)
            .expect("portal receives raw frame");
        assert_eq!(info.len, reply_packed.len());
        assert_eq!(&buf[..info.len], &reply_packed[..]);
        switch.stop().expect("stop switch");
        cleanup(&name);
    }

    #[test]
    fn chunked_large_message_still_reassembles() {
        let name = unique_name("chunked");
        cleanup(&name);
        let mut switch = SwitchHandle::start(SwitchConfig {
            shm_name: name.clone(),
            ttl_ms: 30_000,
            allow: vec!["core".to_owned(), "portal".to_owned()],
            ..SwitchConfig::default()
        })
        .expect("start switch");
        let options = |service: &str| FrontendOptions {
            shm_name: name.clone(),
            service_name: service.to_owned(),
            connect_timeout_ms: 1000,
            recv_timeout_ms: 3000,
            reader_wait_ms: 10,
            chunk_ttl_ms: 30_000,
            ..FrontendOptions::default()
        };
        let core = Frontend::connect(options("core")).expect("connect core");
        let portal = Frontend::connect(options("portal")).expect("connect portal");

        let body_len = 2 * 1024 * 1024; // 2 MiB > 512 KiB => chunked
        let mut msg = protocol::Message::new(protocol::MSG_BUSINESS_REQUEST);
        msg.body = vec![0x5au8; body_len];
        let packed = protocol::pack(&msg).expect("pack 2MiB message");
        portal
            .send("core", &packed, 3000)
            .expect("send chunked message");

        let received = core.recv(3000).expect("core receives reassembled message");
        assert_eq!(received.source, "portal");
        assert_eq!(received.message.body.len(), body_len);
        assert_eq!(received.message.body[..64], vec![0x5au8; 64]);
        switch.stop().expect("stop switch");
        cleanup(&name);
    }

    #[test]
    fn frame_len_peeks_without_consuming() {
        let name = unique_name("framelen");
        cleanup(&name);
        let mut switch = SwitchHandle::start(SwitchConfig {
            shm_name: name.clone(),
            ttl_ms: 30_000,
            allow: vec!["core".to_owned(), "portal".to_owned()],
            ..SwitchConfig::default()
        })
        .expect("start switch");
        // A raw logged-on port has no reader thread, so the peek sees the
        // frame before any consumer drains it.
        let core_options = FrontendOptions {
            shm_name: name.clone(),
            service_name: "core".to_owned(),
            connect_timeout_ms: 1000,
            ..FrontendOptions::default()
        };
        let core_port = PortFrontend::open(&core_options).expect("open raw core port");
        let portal = Frontend::connect(FrontendOptions {
            shm_name: name.clone(),
            service_name: "portal".to_owned(),
            connect_timeout_ms: 1000,
            recv_timeout_ms: 1000,
            reader_wait_ms: 10,
            ..FrontendOptions::default()
        })
        .expect("connect portal");

        let payload = vec![0x42u8; 7 * 1024];
        let mut msg = protocol::Message::new(protocol::MSG_BUSINESS_REQUEST);
        msg.body = payload;
        let packed = protocol::pack(&msg).expect("pack");
        portal.send("core", &packed, 1000).expect("send");

        let mut attempts = 0;
        let peeked = loop {
            if let Some(len) = core_port.frame_len().expect("frame_len call") {
                break len;
            }
            attempts += 1;
            assert!(attempts < 200, "frame never arrived");
            std::thread::sleep(Duration::from_millis(5));
        };
        assert_eq!(peeked, packed.len());

        // The peeked frame must still be deliverable (peek is non-consuming).
        let mut buf = vec![0u8; 1 << 20];
        let frame = core_port
            .recv(&mut buf)
            .expect("recv call")
            .expect("frame still queued after peek");
        assert_eq!(frame.data, packed);
        switch.stop().expect("stop switch");
        cleanup(&name);
    }
}
