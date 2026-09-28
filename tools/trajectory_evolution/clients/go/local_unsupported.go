// go 客户端：通过统一协议收集执行事件并接收策略建议。
// 原生入口复用包内 C ABI；注意句柄生命周期与跨语言内存释放。
// 打点的输入是建议修改前的请求，执行后必须回传实际输入、输出与状态。
// 取消、超时和采集缺口需要显式处理，不能把缺失证据当成正常完成。

//go:build !linux || !cgo

package evolution

// 在启动时加载动态库和稳定 C 接口，缺失符号时拒绝创建半可用句柄。
func openNativeLocal(string, string, string, []byte) (localNative, error) {
	return nil, &Error{Code: "local_library_unsupported"}
}
