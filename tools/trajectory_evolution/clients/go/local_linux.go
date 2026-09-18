//go:build linux && cgo

package evolution

/*
#cgo LDFLAGS: -ldl
#include <dlfcn.h>
#include <stddef.h>
#include <stdlib.h>

typedef void* (*te_go_open_fn)(const char*, const char*, const char*, char**);
typedef char* (*te_go_invoke_fn)(void*, const char*, size_t);
typedef char* (*te_go_metrics_fn)(void*);
typedef void (*te_go_enabled_fn)(void*, int);
typedef void (*te_go_handle_fn)(void*);
typedef void (*te_go_free_fn)(char*);
typedef struct te_go_local {
  void* library; void* handle; te_go_invoke_fn invoke; te_go_invoke_fn manage;
  te_go_metrics_fn metrics; te_go_enabled_fn set_enabled; te_go_handle_fn drain;
  te_go_handle_fn close; te_go_free_fn free_value;
} te_go_local;
static int te_go_symbol(void* library, const char* name, void** output) {
  dlerror(); *output = dlsym(library, name); return *output && !dlerror();
}
static te_go_local* te_go_local_open(const char* path, const char* config,
                                     const char* bearer, const char* options) {
  te_go_local* local = (te_go_local*)calloc(1, sizeof(te_go_local));
  void* open_symbol = NULL;
  if (!local) return NULL;
  local->library = dlopen(path, RTLD_NOW | RTLD_LOCAL);
  if (!local->library || !te_go_symbol(local->library, "te_local_open", &open_symbol) ||
      !te_go_symbol(local->library, "te_local_invoke", (void**)&local->invoke) ||
      !te_go_symbol(local->library, "te_local_manage", (void**)&local->manage) ||
      !te_go_symbol(local->library, "te_local_metrics", (void**)&local->metrics) ||
      !te_go_symbol(local->library, "te_local_set_enabled", (void**)&local->set_enabled) ||
      !te_go_symbol(local->library, "te_local_drain", (void**)&local->drain) ||
      !te_go_symbol(local->library, "te_local_close", (void**)&local->close) ||
      !te_go_symbol(local->library, "te_local_free", (void**)&local->free_value)) {
    if (local->library) dlclose(local->library); free(local); return NULL;
  }
  char* error = NULL;
  local->handle = ((te_go_open_fn)open_symbol)(config, bearer, options, &error);
  if (error) local->free_value(error);
  if (!local->handle) { dlclose(local->library); free(local); return NULL; }
  return local;
}
static char* te_go_local_invoke(te_go_local* l, const char* r, size_t n) { return l && l->handle ? l->invoke(l->handle, r, n) : NULL; }
static char* te_go_local_manage(te_go_local* l, const char* r, size_t n) { return l && l->handle ? l->manage(l->handle, r, n) : NULL; }
static char* te_go_local_metrics(te_go_local* l) { return l && l->handle ? l->metrics(l->handle) : NULL; }
static void te_go_local_free_result(te_go_local* l, char* v) { if (l && v) l->free_value(v); }
static void te_go_local_set_enabled(te_go_local* l, int v) { if (l && l->handle) l->set_enabled(l->handle, v); }
static void te_go_local_drain(te_go_local* l) { if (l && l->handle) l->drain(l->handle); }
static void te_go_local_close(te_go_local* l) { if (!l) return; if (l->handle) l->close(l->handle); if (l->library) dlclose(l->library); free(l); }
*/
import "C"

import "unsafe"

type cgoLocal struct{ handle *C.te_go_local }

func openNativeLocal(library, config, bearer string, options []byte) (localNative, error) {
	libraryText, configText := C.CString(library), C.CString(config)
	bearerText, optionsText := C.CString(bearer), C.CString(string(options))
	defer C.free(unsafe.Pointer(libraryText))
	defer C.free(unsafe.Pointer(configText))
	defer C.free(unsafe.Pointer(bearerText))
	defer C.free(unsafe.Pointer(optionsText))
	handle := C.te_go_local_open(libraryText, configText, bearerText, optionsText)
	if handle == nil {
		return nil, &Error{Code: "local_initialization_failed"}
	}
	return &cgoLocal{handle: handle}, nil
}

func (c *cgoLocal) output(value *C.char) ([]byte, error) {
	if value == nil {
		return nil, &Error{Code: "local_unavailable"}
	}
	defer C.te_go_local_free_result(c.handle, value)
	return []byte(C.GoString(value)), nil
}
func (c *cgoLocal) invoke(body []byte) ([]byte, error) {
	if len(body) == 0 {
		return nil, &Error{Code: "request_invalid"}
	}
	return c.output(C.te_go_local_invoke(c.handle, (*C.char)(unsafe.Pointer(&body[0])), C.size_t(len(body))))
}
func (c *cgoLocal) manage(body []byte) ([]byte, error) {
	if len(body) == 0 {
		return nil, &Error{Code: "request_invalid"}
	}
	return c.output(C.te_go_local_manage(c.handle, (*C.char)(unsafe.Pointer(&body[0])), C.size_t(len(body))))
}
func (c *cgoLocal) metrics() ([]byte, error) { return c.output(C.te_go_local_metrics(c.handle)) }
func (c *cgoLocal) setEnabled(enabled bool) error {
	value := C.int(0)
	if enabled {
		value = 1
	}
	C.te_go_local_set_enabled(c.handle, value)
	return nil
}
func (c *cgoLocal) drain() error { C.te_go_local_drain(c.handle); return nil }
func (c *cgoLocal) close() {
	if c.handle != nil {
		C.te_go_local_close(c.handle)
		c.handle = nil
	}
}
