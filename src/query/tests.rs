use super::*;

#[test]
fn request_golden_is_compatible() {
    let encoded = QueryRequest::new(OP_CORS_ORIGIN_ALLOWED, CACHE_READ_THROUGH, vec!["x".into()])
        .encode()
        .unwrap();
    assert_eq!(encoded, [1, 0, 1, 0, 0, 0, 1, 1, 0, 1, 0, b'x']);
    assert_eq!(QueryRequest::decode(&encoded).unwrap().fields, ["x"]);
}

#[test]
fn hit_miss_bypass_and_errors_are_golden() {
    assert_eq!(
        encode_bool_response(true, CACHE_HIT, 15),
        [1, 0, 0, 1, 15, 0, 0, 0, 1]
    );
    let miss = encode_bytes_response(b"{}", CACHE_MISS, 15);
    assert_eq!(decode_bytes_response(&miss).unwrap().value, b"{}");
    let bypass = encode_bytes_response(b"[]", CACHE_BYPASS_STATUS, 0);
    assert_eq!(
        decode_bytes_response(&bypass).unwrap().cache_status,
        CACHE_BYPASS_STATUS
    );
    let error = encode_error("VALIDATION_ERROR", "bad");
    assert_eq!(
        decode_bytes_response(&error).unwrap_err().code,
        "VALIDATION_ERROR"
    );
}

#[test]
fn operation_ids_and_aliases_stay_stable() {
    for (index, operation) in OPERATIONS.iter().enumerate() {
        assert_eq!(operation.id, index as u32 + 1);
        assert_eq!(operation_by_action(operation.action), Some(*operation));
        assert_eq!(operation_by_action(operation.function), Some(*operation));
    }
}
