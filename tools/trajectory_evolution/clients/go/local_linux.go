// go 客户端：通过统一协议收集执行事件并接收策略建议。
// 原生入口复用包内 C ABI；注意句柄生命周期与跨语言内存释放。
// 打点的输入是建议修改前的请求，执行后必须回传实际输入、输出与状态。
// 取消、超时和采集缺口需要显式处理，不能把缺失证据当成正常完成。

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
// 每次查询前清除动态链接器错误；缺失符号不能作为有效函数指针使用。
static int te_go_symbol(void* library, const char* name, void** output) {
  dlerror();
  *output = dlsym(library, name);
  return *output && !dlerror();
}

// 加载全部稳定 ABI 后才创建运行时，任一步失败都释放已取得的库和临时内存。
// options 和 bearer 仅传给本地库；错误字符串由产生它的库负责释放。
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
    if (local->library) dlclose(local->library);
    free(local);
    return NULL;
  }
  char* error = NULL;
  local->handle = ((te_go_open_fn)open_symbol)(config, bearer, options, &error);
  if (error) local->free_value(error);
  if (!local->handle) {
    dlclose(local->library);
    free(local);
    return NULL;
  }
  return local;
}

// 热路径请求交给本地运行时，返回缓冲区仍属于动态库。
static char* te_go_local_invoke(te_go_local* l, const char* r, size_t n) {
  return l && l->handle ? l->invoke(l->handle, r, n) : NULL;
}

// 管理请求允许阻塞，与 Agent 打点的快速调用入口分开。
static char* te_go_local_manage(te_go_local* l, const char* r, size_t n) {
  return l && l->handle ? l->manage(l->handle, r, n) : NULL;
}

// 查询运行时自身的队列、采集等指标，不触发业务执行。
static char* te_go_local_metrics(te_go_local* l) {
  return l && l->handle ? l->metrics(l->handle) : NULL;
}

// 必须使用与分配端相同的释放函数，不能交给 Go 或系统 free 处理。
static void te_go_local_free_result(te_go_local* l, char* v) {
  if (l && v) l->free_value(v);
}

// 启停组件能力，不结束宿主业务进程。
static void te_go_local_set_enabled(te_go_local* l, int v) {
  if (l && l->handle) l->set_enabled(l->handle, v);
}

// 等待已接收的后台工作排空；关闭或维护时调用，不放入逐次打点路径。
static void te_go_local_drain(te_go_local* l) {
  if (l && l->handle) l->drain(l->handle);
}

// 先销毁运行时，再卸载库，最后释放包装对象，保证析构期间函数仍可调用。
static void te_go_local_close(te_go_local* l) {
  if (!l) return;
  if (l->handle) l->close(l->handle);
  if (l->library) dlclose(l->library);
  free(l);
}
*/
import "C"

import "unsafe"

type cgoLocal struct{ handle *C.te_go_local }

// 在启动时加载动态库和稳定 C 接口，缺失符号时拒绝创建半可用句柄。
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

// 复制或提取当前调用输出，保持跨语言边界的所有权约定。
func (c *cgoLocal) output(value *C.char) ([]byte, error) {
	if value == nil {
		return nil, &Error{Code: "local_unavailable"}
	}
	defer C.te_go_local_free_result(c.handle, value)
	return []byte(C.GoString(value)), nil
}

// 执行一次组件协议调用，检查响应信封并保持请求身份一致。
func (c *cgoLocal) invoke(body []byte) ([]byte, error) {
	if len(body) == 0 {
		return nil, &Error{Code: "request_invalid"}
	}
	return c.output(C.te_go_local_invoke(c.handle, (*C.char)(unsafe.Pointer(&body[0])), C.size_t(len(body))))
}

// 执行评测等阻塞式管理操作，与主 Agent 热路径分离。
func (c *cgoLocal) manage(body []byte) ([]byte, error) {
	if len(body) == 0 {
		return nil, &Error{Code: "request_invalid"}
	}
	return c.output(C.te_go_local_manage(c.handle, (*C.char)(unsafe.Pointer(&body[0])), C.size_t(len(body))))
}

// 读取组件自身的采集和排队指标，不把观测指标当作业务任务得分。
func (c *cgoLocal) metrics() ([]byte, error) { return c.output(C.te_go_local_metrics(c.handle)) }

// 切换组件附加能力，不更改宿主原有业务和安全逻辑。
func (c *cgoLocal) setEnabled(enabled bool) error {
	value := C.int(0)
	if enabled {
		value = 1
	}
	C.te_go_local_set_enabled(c.handle, value)
	return nil
}

// 等待当前待持久化工作完成，用于维护或关闭；不要放在主 Agent 每个调用的热路径。
func (c *cgoLocal) drain() error { C.te_go_local_drain(c.handle); return nil }

// 关闭资源并等待本实现要求的在途操作完成，重复关闭按协议处理。
func (c *cgoLocal) close() {
	if c.handle != nil {
		C.te_go_local_close(c.handle)
		c.handle = nil
	}
}
