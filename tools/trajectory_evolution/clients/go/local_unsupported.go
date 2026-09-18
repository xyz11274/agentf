//go:build !linux || !cgo

package evolution

func openNativeLocal(string, string, string, []byte) (localNative, error) {
	return nil, &Error{Code: "local_library_unsupported"}
}
