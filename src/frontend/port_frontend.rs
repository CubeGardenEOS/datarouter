use std::ffi::CString;
use std::os::raw::{c_char, c_int, c_void};
use std::ptr;

use super::options::FrontendOptions;

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
    pub fn open(options: &FrontendOptions) -> Result<Self, c_int> {
        let shm_name = CString::new(options.shm_name.as_str())
            .map_err(|_| crate::DATAROUTER_ERR_INVALID_ARGUMENT)?;
        let service_name = CString::new(options.service_name.as_str())
            .map_err(|_| crate::DATAROUTER_ERR_INVALID_ARGUMENT)?;
        let c_options = crate::datarouter_options {
            shm_name: shm_name.as_ptr(),
            timeout_ms: options.connect_timeout_ms,
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
