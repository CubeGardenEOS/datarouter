use std::fmt;

use super::{STATUS_ERROR, STATUS_OK, WIRE_VERSION};

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct QueryRequest {
    pub operation_id: u32,
    pub cache_mode: u8,
    pub fields: Vec<String>,
}

impl QueryRequest {
    pub fn new(operation_id: u32, cache_mode: u8, fields: Vec<String>) -> Self {
        Self {
            operation_id,
            cache_mode,
            fields,
        }
    }

    pub fn encode(&self) -> Result<Vec<u8>, QueryProtocolError> {
        if self.fields.len() > usize::from(u16::MAX) {
            return Err(QueryProtocolError::TooManyFields);
        }
        let mut out = Vec::with_capacity(9 + self.fields.iter().map(String::len).sum::<usize>());
        out.extend_from_slice(&WIRE_VERSION.to_le_bytes());
        out.extend_from_slice(&self.operation_id.to_le_bytes());
        out.push(self.cache_mode);
        out.extend_from_slice(&(self.fields.len() as u16).to_le_bytes());
        for field in &self.fields {
            put_string(&mut out, field)?;
        }
        Ok(out)
    }

    pub fn decode(body: &[u8]) -> Result<Self, QueryProtocolError> {
        let mut decoder = Decoder { body, pos: 0 };
        verify_version(decoder.u16()?)?;
        let operation_id = decoder.u32()?;
        let cache_mode = decoder.u8()?;
        let field_count = decoder.u16()?;
        let mut fields = Vec::with_capacity(usize::from(field_count));
        for _ in 0..field_count {
            fields.push(decoder.string()?);
        }
        decoder.finish()?;
        Ok(Self {
            operation_id,
            cache_mode,
            fields,
        })
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct QuerySuccess<T> {
    pub cache_status: u8,
    pub ttl_seconds: u32,
    pub value: T,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct QueryFailure {
    pub code: String,
    pub message: String,
}

pub fn decode_bool_response(body: &[u8]) -> Result<QuerySuccess<bool>, QueryFailure> {
    let (mut decoder, cache_status, ttl_seconds) = decode_response_header(body)?;
    let value = decoder.u8().map_err(protocol_failure)? != 0;
    decoder.finish().map_err(protocol_failure)?;
    Ok(QuerySuccess {
        cache_status,
        ttl_seconds,
        value,
    })
}

pub fn decode_bytes_response(body: &[u8]) -> Result<QuerySuccess<Vec<u8>>, QueryFailure> {
    let (mut decoder, cache_status, ttl_seconds) = decode_response_header(body)?;
    let len = decoder.u32().map_err(protocol_failure)? as usize;
    let value = decoder.take(len).map_err(protocol_failure)?.to_vec();
    decoder.finish().map_err(protocol_failure)?;
    Ok(QuerySuccess {
        cache_status,
        ttl_seconds,
        value,
    })
}

fn decode_response_header(body: &[u8]) -> Result<(Decoder<'_>, u8, u32), QueryFailure> {
    let mut decoder = Decoder { body, pos: 0 };
    verify_version(decoder.u16().map_err(protocol_failure)?).map_err(protocol_failure)?;
    match decoder.u8().map_err(protocol_failure)? {
        STATUS_ERROR => {
            let code = decoder.string().map_err(protocol_failure)?;
            let message = decoder.string().map_err(protocol_failure)?;
            decoder.finish().map_err(protocol_failure)?;
            Err(QueryFailure { code, message })
        }
        STATUS_OK => {
            let cache_status = decoder.u8().map_err(protocol_failure)?;
            let ttl_seconds = decoder.u32().map_err(protocol_failure)?;
            Ok((decoder, cache_status, ttl_seconds))
        }
        status => Err(protocol_failure(QueryProtocolError::InvalidStatus(status))),
    }
}

pub fn encode_bool_response(value: bool, cache_status: u8, ttl_seconds: u32) -> Vec<u8> {
    let mut out = Vec::with_capacity(9);
    encode_success_header(&mut out, cache_status, ttl_seconds);
    out.push(u8::from(value));
    out
}

pub fn encode_bytes_response(value: &[u8], cache_status: u8, ttl_seconds: u32) -> Vec<u8> {
    let len = value.len().min(u32::MAX as usize);
    let mut out = Vec::with_capacity(12 + len);
    encode_success_header(&mut out, cache_status, ttl_seconds);
    out.extend_from_slice(&(len as u32).to_le_bytes());
    out.extend_from_slice(&value[..len]);
    out
}

pub fn encode_error(code: &str, message: &str) -> Vec<u8> {
    let mut out = Vec::with_capacity(8 + code.len() + message.len());
    out.extend_from_slice(&WIRE_VERSION.to_le_bytes());
    out.push(STATUS_ERROR);
    put_truncated_string(&mut out, code);
    put_truncated_string(&mut out, message);
    out
}

fn verify_version(version: u16) -> Result<(), QueryProtocolError> {
    (version == WIRE_VERSION)
        .then_some(())
        .ok_or(QueryProtocolError::UnsupportedVersion(version))
}

fn encode_success_header(out: &mut Vec<u8>, cache_status: u8, ttl_seconds: u32) {
    out.extend_from_slice(&WIRE_VERSION.to_le_bytes());
    out.push(STATUS_OK);
    out.push(cache_status);
    out.extend_from_slice(&ttl_seconds.to_le_bytes());
}

fn put_string(out: &mut Vec<u8>, value: &str) -> Result<(), QueryProtocolError> {
    let len = u16::try_from(value.len()).map_err(|_| QueryProtocolError::FieldTooLong)?;
    out.extend_from_slice(&len.to_le_bytes());
    out.extend_from_slice(value.as_bytes());
    Ok(())
}

fn put_truncated_string(out: &mut Vec<u8>, value: &str) {
    let bytes = value.as_bytes();
    let len = bytes.len().min(usize::from(u16::MAX));
    out.extend_from_slice(&(len as u16).to_le_bytes());
    out.extend_from_slice(&bytes[..len]);
}

fn protocol_failure(error: QueryProtocolError) -> QueryFailure {
    QueryFailure {
        code: "PROTOCOL_ERROR".to_string(),
        message: error.to_string(),
    }
}

struct Decoder<'a> {
    body: &'a [u8],
    pos: usize,
}

impl Decoder<'_> {
    fn u8(&mut self) -> Result<u8, QueryProtocolError> {
        Ok(self.take(1)?[0])
    }
    fn u16(&mut self) -> Result<u16, QueryProtocolError> {
        let bytes = self.take(2)?;
        Ok(u16::from_le_bytes([bytes[0], bytes[1]]))
    }
    fn u32(&mut self) -> Result<u32, QueryProtocolError> {
        let bytes = self.take(4)?;
        Ok(u32::from_le_bytes([bytes[0], bytes[1], bytes[2], bytes[3]]))
    }
    fn string(&mut self) -> Result<String, QueryProtocolError> {
        let len = usize::from(self.u16()?);
        String::from_utf8(self.take(len)?.to_vec()).map_err(|_| QueryProtocolError::InvalidUtf8)
    }
    fn take(&mut self, len: usize) -> Result<&[u8], QueryProtocolError> {
        let end = self
            .pos
            .checked_add(len)
            .ok_or(QueryProtocolError::Truncated)?;
        if end > self.body.len() {
            return Err(QueryProtocolError::Truncated);
        }
        let value = &self.body[self.pos..end];
        self.pos = end;
        Ok(value)
    }
    fn finish(&self) -> Result<(), QueryProtocolError> {
        (self.pos == self.body.len())
            .then_some(())
            .ok_or(QueryProtocolError::TrailingBytes)
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum QueryProtocolError {
    UnsupportedVersion(u16),
    InvalidStatus(u8),
    InvalidUtf8,
    Truncated,
    TrailingBytes,
    TooManyFields,
    FieldTooLong,
}

impl fmt::Display for QueryProtocolError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::UnsupportedVersion(v) => {
                write!(formatter, "unsupported core query wire version {v}")
            }
            Self::InvalidStatus(v) => write!(formatter, "invalid core query response status {v}"),
            Self::InvalidUtf8 => formatter.write_str("query string is not valid utf-8"),
            Self::Truncated => formatter.write_str("unexpected end of query body"),
            Self::TrailingBytes => formatter.write_str("trailing bytes in query body"),
            Self::TooManyFields => formatter.write_str("too many query fields"),
            Self::FieldTooLong => formatter.write_str("query field exceeds u16 wire limit"),
        }
    }
}

impl std::error::Error for QueryProtocolError {}
