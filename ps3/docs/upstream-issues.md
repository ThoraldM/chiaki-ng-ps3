# Bug reports for chiaki-ng (upstream)

File these at https://github.com/streetpea/chiaki-ng/issues/new. Each one stands on its own.
Both are still present in upstream `main` as of 2026-11-10 (line numbers from that version).

---

## Issue 1

**Title:** Stack buffer overflow in `chiaki_rpcrypt_generate_iv` (mbedTLS build)

**Body:**

In the mbedTLS code path of `chiaki_rpcrypt_generate_iv` (`lib/src/rpcrypt.c`, around line 2274), the
HMAC output buffer is declared as

```c
uint8_t hmac[CHIAKI_RPCRYPT_KEY_SIZE];   // 16 bytes
```

but `mbedtls_md_hmac_finish()` with SHA-256 always writes the full 32-byte digest. This overwrites
16 bytes of the stack on every IV generation, which means on every encrypted ctrl message. The OpenSSL
code path (around line 2360) already uses `uint8_t hmac[32];`.

Only builds using mbedTLS (`CHIAKI_LIB_ENABLE_MBEDTLS`, e.g. Switch and other embedded ports) are
affected. Depending on the stack layout it can be silent or corrupt neighbouring variables. In my port to
the PS3 the ctrl thread stopped right after "Ctrl connected", before "Sending ctrl request".

**How to confirm:** build the library with mbedTLS and `-fsanitize=address` and run the rpcrypt unit
tests: ASan reports `stack-buffer-overflow` on `hmac`.

**Fix:** use a 32-byte buffer, as the OpenSSL path does. Only the first `CHIAKI_RPCRYPT_KEY_SIZE` bytes
are used afterwards:

```diff
-	uint8_t hmac[CHIAKI_RPCRYPT_KEY_SIZE];
+	// HMAC-SHA256 always writes 32 bytes; only the first CHIAKI_RPCRYPT_KEY_SIZE are used.
+	uint8_t hmac[32];
```

With this change the unit test suite is clean under ASan/UBSan. Happy to open a PR.

---

## Issue 2

**Title:** Discovery uses `struct sockaddr` as the `recvfrom()` buffer, which truncates the sender address

**Body:**

Both discovery receive loops in `lib/src/discovery.c` (`discovery_thread_func`, around line 343, and
`discovery_thread_func_oneshot`, around line 397) receive into a plain `struct sockaddr`:

```c
struct sockaddr client_addr;
socklen_t client_addr_size = sizeof(client_addr);
CHIAKI_SSIZET_TYPE n = recvfrom(discovery->socket, buf, sizeof(buf) - 1, 0, &client_addr, &client_addr_size);
```

`struct sockaddr` is only guaranteed to be large enough for its family field. It is 16 bytes on common
platforms, so:

- **IPv6** (`sockaddr_in6` is 28 bytes): the address is truncated, so the console address parsed from
  `client_addr` is wrong when discovery runs over IPv6.
- On platforms where `struct sockaddr` declares a flexible `sa_data[]` (for example PSL1GHT on the PS3,
  where `sizeof(struct sockaddr) == 2`), even IPv4 addresses are lost and the console is reported at
  `0.0.0.0`. That's how I found it while porting chiaki-ng to the PS3.

**Fix:** receive into a `struct sockaddr_storage`, which is portable and large enough for any family:

```diff
-		struct sockaddr client_addr;
+		struct sockaddr_storage client_addr;
 		socklen_t client_addr_size = sizeof(client_addr);
-		CHIAKI_SSIZET_TYPE n = recvfrom(discovery->socket, buf, sizeof(buf) - 1, 0, &client_addr, &client_addr_size);
+		CHIAKI_SSIZET_TYPE n = recvfrom(discovery->socket, buf, sizeof(buf) - 1, 0, (struct sockaddr *)&client_addr, &client_addr_size);
 ...
-		err = chiaki_discovery_srch_response_parse(&response, &client_addr, addr_buf, sizeof(addr_buf), buf, n);
+		err = chiaki_discovery_srch_response_parse(&response, (struct sockaddr *)&client_addr, addr_buf, sizeof(addr_buf), buf, n);
```

(same change in both functions). Happy to open a PR.

---

### Not reported (PS3-specific, not upstream bugs)

- `%zu`/`%zx` in log calls crash with the PS3's old newlib (no `%z` support). `%z` is standard C99, so
  that is handled in the port, not upstream.
