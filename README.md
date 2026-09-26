# rinzlib
RinOS用zlib

## Public API contract

| Requirement | Contract |
| --- | --- |
| Purpose | RinZlib is a header-only zlib/DEFLATE decompression and checksum helper for RinOS. |
| Supported API | `rinz.h` exposes raw DEFLATE and zlib-wrapped inflate (`rinz_inflate_raw`, `rinz_inflate`) plus explicitly bounded variants (`rinz_inflate_raw_limited`, `rinz_inflate_limited`), deadline-aware bounded variants (`rinz_inflate_raw_limited_with_deadline`, `rinz_inflate_limited_with_deadline`), and an output-size estimate. `rinz_checksum.h` exposes CRC32 and Adler32 operations. The decoder accepts stored, fixed-Huffman, and dynamic-Huffman DEFLATE blocks and checks the zlib Adler32 trailer. |
| Unsupported API | Compression/encoding is not implemented. Preset dictionaries are rejected; gzip framing and profiles beyond zlib-wrapped DEFLATE are unsupported. |
| ownership | The caller owns source and destination buffers. Output is written only within the supplied capacity and is cleared on decode failure; successful output length is returned through `out_size`. |
| thread-safety | CRC32 and fixed-Huffman tables are initialized once per translation unit with acquire/release atomic state, so independent calls may run concurrently. Caller buffers must not be shared mutably. The implementation uses compiler `__atomic` builtins. |
| limits | The DEFLATE history window is 32 KiB. The limited APIs accept caller-owned input-byte, output-byte, and block-count ceilings; `RINZ_LIMIT_ERROR` is returned when a ceiling is exceeded. The deadline-aware variants additionally poll a caller-owned monotonic deadline predicate and return `RINZ_DEADLINE_ERROR` when it expires. The legacy APIs remain compatible and use unbounded policy values, so callers handling untrusted input should use the limited variants. `rinz_inflate_bound` is only an estimate, not a safety limit. |
| errors | Operations return `RINZ_OK`, `RINZ_ERROR`, `RINZ_DATA_ERROR`, `RINZ_BUF_ERROR`, `RINZ_LIMIT_ERROR`, or `RINZ_DEADLINE_ERROR`. Invalid arguments, malformed streams, checksum mismatch, insufficient destination capacity, explicit policy violations, and expired deadlines fail; callers must discard output unless status is `RINZ_OK`. |
| ABI stability | The API consists of static inline C declarations and structs in public headers, with no separately versioned binary ABI. Consumers compile against the exact header version they use. |
| security | Treat compressed input as untrusted and set explicit input, output, and block-count budgets with `RinzInflateLimits`. Use the deadline-aware variants with a cheap monotonic deadline predicate when a wall-clock budget is required; a bounded output buffer or block count alone does not provide that guarantee. |
| build | Include `rinz.h` and/or `rinz_checksum.h` from the RinOS build. No standalone build or install system is provided. |
| test | Build `tests/rinzlib_test.c` with a C11 compiler for the direct contract test. The fuzz entrypoint is `fuzz/rinzlib_fuzzer.c`; consuming image and data-processing targets remain additional validation. |
