use std::ffi::CString;
use std::os::raw::{c_char, c_int, c_void};
use std::ptr;

use super::options::FrontendOptions;

/// One channel of shared memory: the largest possible single-frame payload
/// plus the 8-byte frame header. Receive buffers are sized to this.
pub const RAW_RECV_BUF_SIZE: usize = crate::DATAROUTER_CHANNEL_SIZE as usize;

#[derive(Debug)]
pub struct RawFrame {
    pub source: [c_char; crate::DATAROUTER_SERVICE_NAME_MAX + 1],
    pub data: Vec<u8>,
}

#[derive(Debug)]
pub struct PortFrontend {
    port: *mut crate::datarouter_port,
}

unsafe impl Send for PortFrontend {}

impl PortFrontend {
    /// Open the shared-memory mapping without logging on. Used by the switch
    /// frontend to probe liveness and snapshot capacity/registry without
    /// consuming a registration slot.
    pub fn open_raw(options: &FrontendOptions) -> Result<Self, c_int> {
        let shm_name = CString::new(options.shm_name.as_str())
            .map_err(|_| crate::DATAROUTER_ERR_INVALID_ARGUMENT)?;
        let auth_token = options
            .auth_token
            .as_deref()
            .map(CString::new)
            .transpose()
            .map_err(|_| crate::DATAROUTER_ERR_INVALID_ARGUMENT)?;
        let c_options = crate::datarouter_options {
            shm_name: shm_name.as_ptr(),
            timeout_ms: options.connect_timeout_ms,
            auth_token: auth_token
                .as_ref()
                .map_or(std::ptr::null(), |token| token.as_ptr()),
        };
        let mut port = ptr::null_mut();
        status_to_result(unsafe { crate::datarouter_open(&c_options, &mut port) })?;
        Ok(Self { port })
    }

    pub fn open(options: &FrontendOptions) -> Result<Self, c_int> {
        let shm_name = CString::new(options.shm_name.as_str())
            .map_err(|_| crate::DATAROUTER_ERR_INVALID_ARGUMENT)?;
        let service_name = CString::new(options.service_name.as_str())
            .map_err(|_| crate::DATAROUTER_ERR_INVALID_ARGUMENT)?;
        let auth_token = options
            .auth_token
            .as_deref()
            .map(CString::new)
            .transpose()
            .map_err(|_| crate::DATAROUTER_ERR_INVALID_ARGUMENT)?;
        let c_options = crate::datarouter_options {
            shm_name: shm_name.as_ptr(),
            timeout_ms: options.connect_timeout_ms,
            auth_token: auth_token
                .as_ref()
                .map_or(std::ptr::null(), |token| token.as_ptr()),
        };
        let mut port = ptr::null_mut();
        status_to_result(unsafe { crate::datarouter_open(&c_options, &mut port) })?;

        let mut errbuf = vec![0 as c_char; 256];
        let code = unsafe {
            crate::datarouter_logon(
                port,
                service_name.as_ptr(),
                errbuf.as_mut_ptr(),
                errbuf.len(),
            )
        };
        if code != crate::DATAROUTER_OK {
            unsafe { crate::datarouter_close(port) };
            return Err(code);
        }
        if let Err(code) = status_to_result(unsafe { crate::datarouter_touch(port) }) {
            unsafe { crate::datarouter_close(port) };
            return Err(code);
        }

        Ok(Self { port })
    }

    pub fn wait(&self, timeout_ms: u32) -> c_int {
        unsafe { crate::datarouter_wait(self.port, timeout_ms) }
    }

    pub fn touch(&self) -> Result<(), c_int> {
        status_to_result(unsafe { crate::datarouter_touch(self.port) })
    }

    pub fn ttl_ms(&self) -> u32 {
        let ttl = unsafe { crate::datarouter_ttl_ms(self.port) };
        if ttl == 0 {
            crate::DATAROUTER_DEFAULT_TIMEOUT_MS
        } else {
            ttl
        }
    }

    pub fn transport_stats(&self) -> Result<crate::datarouter_transport_stats, c_int> {
        let mut stats = crate::datarouter_transport_stats::default();
        status_to_result(unsafe {
            crate::datarouter_transport_stats_snapshot(self.port, &mut stats)
        })?;
        Ok(stats)
    }

    pub fn send(&self, target_service: &str, data: &[u8]) -> Result<(), c_int> {
        let target =
            CString::new(target_service).map_err(|_| crate::DATAROUTER_ERR_INVALID_ARGUMENT)?;
        status_to_result(unsafe {
            crate::datarouter_send(
                self.port,
                target.as_ptr(),
                data.as_ptr().cast::<c_void>(),
                data.len(),
            )
        })
    }

    pub fn send_to_c_source(
        &self,
        source: &[c_char; crate::DATAROUTER_SERVICE_NAME_MAX + 1],
        data: &[u8],
    ) -> Result<(), c_int> {
        status_to_result(unsafe {
            crate::datarouter_send(
                self.port,
                source.as_ptr(),
                data.as_ptr().cast::<c_void>(),
                data.len(),
            )
        })
    }

    pub fn recv(&self, recv_buf: &mut [u8]) -> Result<Option<RawFrame>, c_int> {
        let mut info = crate::datarouter_message_info {
            source: [0; crate::DATAROUTER_SERVICE_NAME_MAX + 1],
            len: 0,
        };
        let rc = unsafe {
            crate::datarouter_recv(
                self.port,
                recv_buf.as_mut_ptr().cast::<c_void>(),
                recv_buf.len(),
                &mut info,
            )
        };
        match rc {
            crate::DATAROUTER_OK if info.len <= recv_buf.len() => Ok(Some(RawFrame {
                source: info.source,
                data: recv_buf[..info.len].to_vec(),
            })),
            crate::DATAROUTER_OK => Err(crate::DATAROUTER_ERR_PROTOCOL),
            crate::DATAROUTER_ERR_AGAIN | crate::DATAROUTER_ERR_TIMEOUT => Ok(None),
            code => Err(code),
        }
    }

    /// Receive one frame with no extra copies. C writes directly into the
    /// caller buffer's spare capacity (a single ring -> buffer copy), and the
    /// buffer is handed to the queue as-is (no probe, no to_vec, no
    /// unpack/repack). The caller should hand a pooled buffer so the hand-off
    /// costs no allocation in steady state.
    ///
    /// # Safety
    /// C's `datarouter_recv` writes exactly `info.len` bytes on success, so
    /// `set_len(info.len)` over those bytes is sound.
    pub fn recv_into(&self, buf: &mut Vec<u8>) -> Result<Option<RawFrame>, c_int> {
        if buf.capacity() < RAW_RECV_BUF_SIZE {
            buf.reserve(RAW_RECV_BUF_SIZE - buf.capacity());
        }
        let spare = buf.spare_capacity_mut();
        let mut info = crate::datarouter_message_info {
            source: [0; crate::DATAROUTER_SERVICE_NAME_MAX + 1],
            len: 0,
        };
        let rc = unsafe {
            crate::datarouter_recv(
                self.port,
                spare.as_mut_ptr().cast::<c_void>(),
                spare.len(),
                &mut info,
            )
        };
        match rc {
            crate::DATAROUTER_OK if info.len <= spare.len() => {
                // SAFETY: C wrote exactly `info.len` initialized bytes into
                // the spare capacity above.
                unsafe { buf.set_len(info.len) };
                Ok(Some(RawFrame {
                    source: info.source,
                    data: std::mem::take(buf),
                }))
            }
            crate::DATAROUTER_OK => Err(crate::DATAROUTER_ERR_PROTOCOL),
            crate::DATAROUTER_ERR_AGAIN | crate::DATAROUTER_ERR_TIMEOUT => Ok(None),
            code => Err(code),
        }
    }

    pub fn capacity(&self) -> Result<(u32, u32), c_int> {
        let max_ports = unsafe { crate::datarouter_max_ports(self.port) };
        let active = unsafe { crate::datarouter_active_ports(self.port) };
        if max_ports == 0 {
            return Err(crate::DATAROUTER_ERR_INTERNAL);
        }
        Ok((max_ports, active))
    }

    /// Peek the next frame's payload length without consuming it
    /// (`None` when the RX channel is empty). Useful for sizing buffers
    /// before a zero-copy receive.
    pub fn frame_len(&self) -> Result<Option<usize>, c_int> {
        let mut payload_len = 0usize;
        match unsafe { crate::datarouter_frame_len(self.port, &mut payload_len) } {
            crate::DATAROUTER_OK => Ok(Some(payload_len)),
            crate::DATAROUTER_ERR_AGAIN | crate::DATAROUTER_ERR_TIMEOUT => Ok(None),
            code => Err(code),
        }
    }

    pub fn registry_snapshot(
        &self,
        entries: &mut [crate::datarouter_registry_snapshot_entry],
        count: &mut usize,
    ) -> c_int {
        unsafe {
            crate::datarouter_registry_snapshot(
                self.port,
                entries.as_mut_ptr(),
                entries.len(),
                count,
            )
        }
    }

    #[cfg(test)]
    pub fn test_release_self_slot(&self) -> Result<(), c_int> {
        unsafe extern "C" {
            fn datarouter_test_release_self_slot(port: *mut crate::datarouter_port) -> c_int;
        }
        status_to_result(unsafe { datarouter_test_release_self_slot(self.port) })
    }
}

impl Drop for PortFrontend {
    fn drop(&mut self) {
        unsafe { crate::datarouter_close(self.port) };
    }
}

fn status_to_result(code: c_int) -> Result<(), c_int> {
    if code == crate::DATAROUTER_OK {
        Ok(())
    } else {
        Err(code)
    }
}
