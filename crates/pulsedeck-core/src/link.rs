//! Byte-level transport: the `Link` trait, the real serial implementation and port discovery.

use crate::protocol::Protocol;
use serde::{Deserialize, Serialize};
use std::io::{self, Read, Write};
use std::time::{Duration, Instant};

/// A line-oriented duplex channel to the pedal.
pub trait Link: Send {
    /// Write one command line; the terminating `\n` is added here.
    fn write_line(&mut self, line: &str) -> io::Result<()>;
    /// Read one line (trailing `\r\n` stripped). `Ok(None)` = nothing arrived within `timeout`.
    /// An `Err` means the link is gone.
    fn read_line(&mut self, timeout: Duration) -> io::Result<Option<String>>;
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct PortCandidate {
    pub name: String,
    pub vid: u16,
    pub pid: u16,
    pub product: Option<String>,
    pub serial_number: Option<String>,
}

/// Where candidate ports come from and how they are opened (swappable for the simulator).
pub trait Connector: Send + Sync {
    fn candidates(&self) -> Vec<PortCandidate>;
    fn open(&self, port: &PortCandidate) -> io::Result<Box<dyn Link>>;
}

// ---------------------------------------------------------------------------------- serial

pub struct SerialLink {
    port: Box<dyn serialport::SerialPort>,
    buf: Vec<u8>,
}

impl SerialLink {
    pub fn open(name: &str, baud: u32) -> io::Result<Self> {
        let mut port = serialport::new(name, baud)
            .timeout(Duration::from_millis(50))
            .open()
            .map_err(|e| io::Error::other(e.to_string()))?;
        // CDC-ACM firmware commonly gates its TX on the DTR line-state bit.
        let _ = port.write_data_terminal_ready(true);
        let _ = port.clear(serialport::ClearBuffer::All);
        Ok(Self {
            port,
            buf: Vec::with_capacity(512),
        })
    }

    fn pop_line(&mut self) -> Option<String> {
        let nl = self.buf.iter().position(|b| *b == b'\n')?;
        let mut line: Vec<u8> = self.buf.drain(..=nl).collect();
        line.pop();
        if line.last() == Some(&b'\r') {
            line.pop();
        }
        Some(String::from_utf8_lossy(&line).into_owned())
    }
}

impl Link for SerialLink {
    fn write_line(&mut self, line: &str) -> io::Result<()> {
        let mut data = Vec::with_capacity(line.len() + 1);
        data.extend_from_slice(line.as_bytes());
        data.push(b'\n');
        self.port.write_all(&data)?;
        self.port.flush()
    }

    fn read_line(&mut self, timeout: Duration) -> io::Result<Option<String>> {
        let deadline = Instant::now() + timeout;
        let mut tmp = [0u8; 512];
        loop {
            if let Some(l) = self.pop_line() {
                return Ok(Some(l));
            }
            let now = Instant::now();
            if now >= deadline {
                return Ok(None);
            }
            let slice = (deadline - now)
                .min(Duration::from_millis(50))
                .max(Duration::from_millis(1));
            let _ = self.port.set_timeout(slice);
            match self.port.read(&mut tmp) {
                Ok(0) => {}
                Ok(n) => self.buf.extend_from_slice(&tmp[..n]),
                Err(e)
                    if e.kind() == io::ErrorKind::TimedOut
                        || e.kind() == io::ErrorKind::WouldBlock => {}
                Err(e) => return Err(e),
            }
            if self.buf.len() > 64 * 1024 {
                // garbage without newlines; keep memory bounded
                self.buf.clear();
            }
        }
    }
}

pub struct SerialConnector {
    pub protocol: Protocol,
}

impl SerialConnector {
    pub fn new(protocol: Protocol) -> Self {
        Self { protocol }
    }
}

impl Connector for SerialConnector {
    fn candidates(&self) -> Vec<PortCandidate> {
        list_candidates(self.protocol.usb_vendor_id)
    }

    fn open(&self, port: &PortCandidate) -> io::Result<Box<dyn Link>> {
        Ok(Box::new(SerialLink::open(
            &port.name,
            self.protocol.baud_rate,
        )?))
    }
}

/// USB serial ports whose vendor id matches (any product id).
pub fn list_candidates(vid: u16) -> Vec<PortCandidate> {
    let mut out: Vec<PortCandidate> = serialport::available_ports()
        .unwrap_or_default()
        .into_iter()
        .filter_map(|p| match p.port_type {
            serialport::SerialPortType::UsbPort(u) if u.vid == vid => Some(PortCandidate {
                name: p.port_name,
                vid: u.vid,
                pid: u.pid,
                product: u.product,
                serial_number: u.serial_number,
            }),
            _ => None,
        })
        .collect();
    out.sort_by(|a, b| a.name.cmp(&b.name));
    out
}
