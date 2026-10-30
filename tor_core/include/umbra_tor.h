// C ABI of the umbra_tor library (Rust, Arti). Loaded at runtime by the
// browser with base::LoadNativeLibrary; nothing links against it directly.
#ifndef UMBRA_TOR_CORE_INCLUDE_UMBRA_TOR_H_
#define UMBRA_TOR_CORE_INCLUDE_UMBRA_TOR_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UMBRA_TOR_ABI_VERSION 1u

#define UMBRA_TOR_OK 0
#define UMBRA_TOR_ERR_BAD_ARGS (-1)
#define UMBRA_TOR_ERR_BIND (-2)
#define UMBRA_TOR_ERR_THREAD (-3)
#define UMBRA_TOR_ERR_PANIC (-4)

#define UMBRA_TOR_PHASE_FAILED (-1)
#define UMBRA_TOR_PHASE_STARTING 0
#define UMBRA_TOR_PHASE_BOOTSTRAPPING 1
#define UMBRA_TOR_PHASE_READY 2
#define UMBRA_TOR_PHASE_STOPPED 3

typedef uint32_t (*umbra_tor_abi_version_fn)(void);
typedef int32_t (*umbra_tor_start_fn)(const char* state_dir,
                                      const char* cache_dir,
                                      uint16_t* out_port);
typedef int32_t (*umbra_tor_status_fn)(int32_t* out_percent);
typedef size_t (*umbra_tor_last_error_fn)(char* buf, size_t len);
/* Optional (look it up; older libraries lack it): stops Tor and returns once
 * it has; umbra_tor_start may then start it again on a new port. */
typedef int32_t (*umbra_tor_stop_fn)(void);

uint32_t umbra_tor_abi_version(void);
int32_t umbra_tor_start(const char* state_dir,
                        const char* cache_dir,
                        uint16_t* out_port);
int32_t umbra_tor_status(int32_t* out_percent);
size_t umbra_tor_last_error(char* buf, size_t len);
int32_t umbra_tor_stop(void);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // UMBRA_TOR_CORE_INCLUDE_UMBRA_TOR_H_
