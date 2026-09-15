"""Local command isolation, independent of the HTTP server's own privileges.

Each command starts a fresh helper, restricts itself, then execs bash. No
preexec_fn or process-wide environment mutation in the threaded HTTP server.
Linux Landlock ABI >= 3 and libseccomp are required; errors fail closed.
"""

from __future__ import annotations

import ctypes
import errno
import json
import math
import os
from pathlib import Path
import platform
import signal
import subprocess
import sys
import tempfile


def run_local_command(command: str, workdir: str, timeout: float = 30) -> dict:
    """Execute inside a workspace or report failure; never fall back to host."""
    from agentflow_runtime.security import safe_subprocess_environment

    result = {"exit_code": 1, "stdout": "", "stderr": "", "truncated": False}
    try:
        if not isinstance(command, str) or not command.strip():
            raise ValueError("command must not be empty")
        if isinstance(timeout, bool) or not math.isfinite(timeout) or timeout <= 0:
            raise ValueError("timeout must be positive and finite")
        root = Path(workdir).resolve(strict=True)
        if (not root.is_dir() or root == Path(root.anchor)
                or root in Path(__file__).resolve().parents
                or root == Path(sys.prefix) or root in Path(sys.prefix).parents):
            raise ValueError("invalid local command workspace")
        env = safe_subprocess_environment()
        # Shell/Python startup configuration must not come from the host home.
        env.pop("PYTHONPATH", None)
        env.update(HOME=str(root), TMPDIR=str(root), TMP=str(root), TEMP=str(root))
        with tempfile.TemporaryFile() as out, tempfile.TemporaryFile() as err:
            child = subprocess.Popen(
                [sys.executable, "-I", str(Path(__file__).resolve())],
                stdin=subprocess.PIPE, stdout=out, stderr=err,
                cwd=root, env=env, start_new_session=True, close_fds=True,
            )
            try:
                child.communicate(json.dumps({"command": command, "root": str(root)}).encode(), timeout=timeout)
                result["exit_code"] = child.returncode
            except subprocess.TimeoutExpired:
                result["stderr"] = f"Command timed out after {timeout:g}s\n"
                result["exit_code"] = -1
            finally:
                # Clean background children on success too. The child restriction
                # disallows setsid/setpgid, so commands cannot escape this group.
                try:
                    os.killpg(child.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                child.wait()
            for name, stream in (("stdout", out), ("stderr", err)):
                stream.seek(0)
                data = stream.read(8 * 1024 * 1024 + 1)
                result["truncated"] |= len(data) > 8 * 1024 * 1024
                result[name] += data[:8 * 1024 * 1024].decode("utf-8", errors="replace")
    except (OSError, ValueError, TypeError) as exc:
        result["stderr"] = f"Local command failed: {exc}"
    return result


def _confine(root: Path) -> None:
    """Install filesystem and process restrictions before user code runs."""
    if sys.platform != "linux" or platform.machine() not in {"x86_64", "aarch64"}:
        raise RuntimeError("local bash requires Linux Landlock ABI 3 and libseccomp")
    libc = ctypes.CDLL(None, use_errno=True)
    libc.syscall.restype = ctypes.c_long
    if libc.syscall(444, 0, 0, 1) < 3:  # landlock_create_ruleset(VERSION)
        raise RuntimeError("local bash requires Landlock ABI 3 (no unsafe fallback)")

    class Ruleset(ctypes.Structure):
        _fields_ = [("handled_access_fs", ctypes.c_uint64)]

    class PathRule(ctypes.Structure):
        _pack_ = 1
        _fields_ = [("allowed_access", ctypes.c_uint64), ("parent_fd", ctypes.c_int)]

    handled = (1 << 15) - 1  # ABI 3: includes REFER and TRUNCATE.
    read = (1 << 0) | (1 << 2) | (1 << 3)
    write = handled & ~((1 << 6) | (1 << 9) | (1 << 10))
    ruleset = Ruleset(handled)
    fd = libc.syscall(444, ctypes.byref(ruleset), ctypes.sizeof(ruleset), 0)
    if fd < 0:
        raise OSError(ctypes.get_errno(), "cannot create local filesystem restriction")
    try:
        def allow(path: Path, access: int) -> None:
            if not path.exists():
                return
            path_fd = os.open(path, os.O_PATH | os.O_CLOEXEC)
            try:
                if not path.is_dir():
                    access &= (1 << 0) | (1 << 1) | (1 << 2) | (1 << 14)
                rule = PathRule(access, path_fd)
                if libc.syscall(445, fd, 1, ctypes.byref(rule), 0) < 0:
                    raise OSError(ctypes.get_errno(), "cannot add local filesystem rule")
            finally:
                os.close(path_fd)

        # Runtime trees are read-only; /proc, homes, other workspaces and the
        # agent package are not granted. Symlinks do not add read permissions.
        for path in ("/usr", "/bin", "/sbin", "/lib", "/lib64"):
            allow(Path(path), read)
        for prefix in {Path(sys.prefix), Path(sys.base_prefix)}:
            if prefix != Path("/usr"):
                for name in ("bin", "lib", "lib64"):
                    allow(prefix / name, read)
        for path in ("/etc/ld.so.cache", "/etc/resolv.conf", "/etc/hosts",
                     "/etc/nsswitch.conf", "/etc/gai.conf", "/etc/localtime", "/etc/ssl/certs"):
            allow(Path(path), read)
        for path in ("/dev/null", "/dev/zero", "/dev/urandom", "/dev/random"):
            allow(Path(path), (1 << 1) | (1 << 2))
        allow(root, write)
        if libc.prctl(38, 1, 0, 0, 0) != 0:  # PR_SET_NO_NEW_PRIVS
            raise OSError(ctypes.get_errno(), "cannot prevent privilege escalation")
        if libc.syscall(446, fd, 0) < 0:  # landlock_restrict_self
            raise OSError(ctypes.get_errno(), "cannot restrict local filesystem")
    finally:
        os.close(fd)
    _restrict_processes()


def _restrict_processes() -> None:
    """Cover process/metadata operations not handled by Landlock ABI 3."""
    seccomp = ctypes.CDLL("libseccomp.so.2", use_errno=True)
    seccomp.seccomp_init.argtypes = [ctypes.c_uint32]
    seccomp.seccomp_init.restype = ctypes.c_void_p
    seccomp.seccomp_syscall_resolve_name.argtypes = [ctypes.c_char_p]
    seccomp.seccomp_rule_add.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_int, ctypes.c_uint]
    seccomp.seccomp_load.argtypes = [ctypes.c_void_p]
    seccomp.seccomp_release.argtypes = [ctypes.c_void_p]
    ctx = seccomp.seccomp_init(0x7FFF0000)  # SCMP_ACT_ALLOW
    if not ctx:
        raise RuntimeError("cannot initialize local process restriction")
    try:
        for name in (
            "ptrace", "process_vm_readv", "process_vm_writev", "pidfd_getfd",
            "kill", "tkill", "tgkill", "pidfd_send_signal", "setsid", "setpgid",
            "mount", "umount2", "pivot_root", "chroot", "setns", "unshare",
            "open_by_handle_at", "name_to_handle_at", "bpf", "userfaultfd",
            "io_uring_setup", "io_uring_enter", "io_uring_register",
            "chmod", "fchmod", "fchmodat", "fchmodat2", "chown", "fchown",
            "lchown", "fchownat", "utime", "utimes", "futimesat", "utimensat",
            "setxattr", "lsetxattr", "fsetxattr", "removexattr", "lremovexattr", "fremovexattr",
            "capset", "setuid", "setgid", "setreuid", "setregid", "setresuid", "setresgid",
            "setfsuid", "setfsgid", "setgroups", "reboot", "kexec_load", "kexec_file_load",
            "init_module", "finit_module", "delete_module", "swapon", "swapoff",
            "shmget", "shmat", "shmctl", "msgget", "msgsnd", "msgrcv", "msgctl",
            "semget", "semop", "semtimedop", "semctl", "keyctl", "add_key", "request_key",
        ):
            number = seccomp.seccomp_syscall_resolve_name(name.encode())
            if number >= 0 and seccomp.seccomp_rule_add(ctx, 0x50000 | errno.EPERM, number, 0) != 0:
                raise RuntimeError("cannot install local process rule")

        class Comparison(ctypes.Structure):
            _fields_ = [("arg", ctypes.c_uint), ("op", ctypes.c_int),
                        ("a", ctypes.c_uint64), ("b", ctypes.c_uint64)]

        # TCP/UDP networking stays available; no Unix/packet/netlink sockets
        # into the host's local control plane and no raw-IP sniffing.
        socket_number = seccomp.seccomp_syscall_resolve_name(b"socket")
        for family in (0, 1, 16, 17, 40):
            condition = Comparison(0, 4, family, 0)  # SCMP_CMP_EQ
            if seccomp.seccomp_rule_add(ctx, 0x50000 | errno.EPERM, socket_number, 1, condition) != 0:
                raise RuntimeError("cannot restrict local socket families")
        condition = Comparison(1, 7, 15, 3)  # SCMP_CMP_MASKED_EQ: SOCK_RAW
        if seccomp.seccomp_rule_add(ctx, 0x50000 | errno.EPERM, socket_number, 1, condition) != 0:
            raise RuntimeError("cannot restrict raw sockets")
        if seccomp.seccomp_load(ctx) != 0:
            raise RuntimeError("cannot enforce local process restriction")
    finally:
        seccomp.seccomp_release(ctx)


def _main() -> None:
    try:
        request = json.load(sys.stdin)
        root = Path(request["root"]).resolve(strict=True)
        _confine(root)
        os.execve("/bin/bash", ["bash", "--noprofile", "--norc", "-c", request["command"]], os.environ)
    except Exception as exc:
        print(f"security_policy_blocked: local command isolation unavailable: {exc}", file=sys.stderr)
        sys.exit(126)


if __name__ == "__main__":
    _main()
