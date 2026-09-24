# rinzlib
RinOS用zlib

## Public API contract

| Requirement | Contract |
| --- | --- |
| Purpose | RinZlib is a header-only zlib/DEFLATE decompression and checksum helper for RinOS. |
| Supported API | `rinz.h` exposes raw DEFLATE and zlib-wrapped inflate (`rinz_inflate_raw`, `rinz_inflate`) plus an output-size estimate. `rinz_checksum.h` exposes CRC32 and Adler32 operations. The decoder accepts stored, fixed-Huffman, and dynamic-Huffman DEFLATE blocks and checks the zlib Adler32 trailer. |
| Unsupported API | Compression/encoding is not implemented. Preset dictionaries are rejected; gzip framing and profiles beyond zlib-wrapped DEFLATE are unsupported. |
| ownership | The caller owns source and destination buffers. Output is written only within the supplied capacity and is cleared on decode failure; successful output length is returned through `out_size`. |
| thread-safety | CRC32 and fixed-Huffman tables are initialized once per translation unit with acquire/release atomic state, so independent calls may run concurrently. Caller buffers must not be shared mutably. The implementation uses compiler `__atomic` builtins. |
| limits | The DEFLATE history window is 32 KiB. The API has no fixed input/output byte ceiling; callers must enforce their own resource bounds and supply an adequately bounded output buffer. `rinz_inflate_bound` is only an estimate, not a safety limit. |
| errors | Operations return `RINZ_OK`, `RINZ_ERROR`, `RINZ_DATA_ERROR`, or `RINZ_BUF_ERROR`. Invalid arguments, malformed streams, checksum mismatch, and insufficient destination capacity fail; callers must discard output unless status is `RINZ_OK`. |
| ABI stability | The API consists of static inline C declarations and structs in public headers, with no separately versioned binary ABI. Consumers compile against the exact header version they use. |
| security | Treat compressed input as untrusted and set explicit caller-side input, output, and time budgets. A bounded output buffer does not bound CPU time for every input. |
| build | Include `rinz.h` and/or `rinz_checksum.h` from the RinOS build. No standalone build or install system is provided. |
| test | No standalone test command is documented in this repository. Validate through the consuming image and data-processing targets. |
