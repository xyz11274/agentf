package agentflow

import (
"encoding/base64"
"fmt"
"os"
"path/filepath"
"time"
"net/url"
)

type FileDataBus struct {
Root string
}

func NewFileDataBus(root string) *FileDataBus {
if root == "" {
root = os.Getenv("AF_DATABUS_DIR")
}
os.MkdirAll(filepath.Join(root, "kv"), 0o755)
os.MkdirAll(filepath.Join(root, "events"), 0o755)
return &FileDataBus{Root: root}
}

func (b *FileDataBus) keyPath(key string) string {
return filepath.Join(b.Root, "kv", url.PathEscape(key)+".kv")
}

func (b *FileDataBus) Set(key string, value string, valueType string) error {
if valueType == "" {
valueType = "string"
}
encoded := base64.StdEncoding.EncodeToString([]byte(value))
content := fmt.Sprintf("key=%s\ntype=%s\nvalue_b64=%s\ntimestamp_ms=%d\n", key, valueType, encoded, time.Now().UnixMilli())
return os.WriteFile(b.keyPath(key), []byte(content), 0o644)
}
