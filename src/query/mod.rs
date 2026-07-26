//! Byte-compatible codec for the internal `Core.Query.*` protocol.

mod codec;
mod operations;

pub use codec::{
    QueryFailure, QueryProtocolError, QueryRequest, QuerySuccess, decode_bool_response,
    decode_bytes_response, encode_bool_response, encode_bytes_response, encode_error,
};
pub use operations::*;

#[cfg(test)]
mod tests;
