"""Transport adapters for the virtual SupraMatic 4 master."""

from __future__ import annotations

import os
import pty
import select
import socket
import subprocess
import termios
import tty
from pathlib import Path
from types import TracebackType
from typing import Protocol


ROOT = Path(__file__).resolve().parents[2]
HOST_RESPONDER = ROOT / "tests" / "hcp2" / "build" / "host_responder"


class Transport(Protocol):
    def write(self, data: bytes) -> None: ...

    def read_available(self, timeout: float) -> bytes: ...

    def command(self, line: str) -> str: ...

    def close(self) -> None: ...


def build_host_responder() -> None:
    subprocess.run(["make", "-s", "-C", "tests/hcp2", "host_responder"], cwd=ROOT, check=True)


def _set_raw(fd: int) -> None:
    try:
        tty.setraw(fd, termios.TCSANOW)
    except termios.error:
        pass


class HostProcessTransport:
    def __init__(self, proc: subprocess.Popen[bytes], fd: int) -> None:
        self.proc = proc
        self.fd = fd
        os.set_blocking(self.fd, False)

    def _read_ready(self) -> str:
        assert self.proc.stdout is not None
        ready = self.proc.stdout.readline().decode("ascii", errors="replace").strip()
        if not ready.startswith("READY"):
            raise RuntimeError(f"host_responder did not start: {ready!r}")
        return ready

    def write(self, data: bytes) -> None:
        offset = 0
        while offset < len(data):
            try:
                offset += os.write(self.fd, data[offset:])
            except BlockingIOError:
                select.select([], [self.fd], [], 0.05)

    def read_available(self, timeout: float) -> bytes:
        readable, _, _ = select.select([self.fd], [], [], max(timeout, 0.0))
        if not readable:
            return b""
        try:
            return os.read(self.fd, 4096)
        except BlockingIOError:
            return b""

    def command(self, line: str) -> str:
        if self.proc.stdin is None or self.proc.stdout is None:
            raise RuntimeError("host_responder command pipe is closed")
        self.proc.stdin.write((line.rstrip() + "\n").encode("ascii"))
        self.proc.stdin.flush()
        return self.proc.stdout.readline().decode("ascii", errors="replace").strip()

    def close(self) -> None:
        try:
            if self.proc.stdin is not None and self.proc.poll() is None:
                self.command("quit")
        except Exception:
            pass
        try:
            os.close(self.fd)
        except OSError:
            pass
        if self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=2)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait(timeout=2)

    def __enter__(self) -> "HostProcessTransport":
        return self

    def __exit__(
        self,
        exc_type: type[BaseException] | None,
        exc: BaseException | None,
        tb: TracebackType | None,
    ) -> None:
        self.close()


class PtyHostTransport(HostProcessTransport):
    def __init__(self, response_delay_us: int = 4500, slave_id: int = 2) -> None:
        build_host_responder()
        master_fd, slave_fd = pty.openpty()
        _set_raw(master_fd)
        slave_path = os.ttyname(slave_fd)
        proc = subprocess.Popen(
            [
                str(HOST_RESPONDER),
                "--device",
                slave_path,
                "--slave-id",
                str(slave_id),
                "--response-delay-us",
                str(response_delay_us),
            ],
            cwd=ROOT,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        os.close(slave_fd)
        super().__init__(proc, master_fd)
        self._read_ready()


class SocketPairHostTransport(HostProcessTransport):
    def __init__(self, response_delay_us: int = 4500, slave_id: int = 2) -> None:
        build_host_responder()
        parent, child = socket.socketpair()
        parent.setblocking(False)
        child.setblocking(False)
        parent_fd = parent.detach()
        proc = subprocess.Popen(
            [
                str(HOST_RESPONDER),
                "--fd",
                str(child.fileno()),
                "--slave-id",
                str(slave_id),
                "--response-delay-us",
                str(response_delay_us),
            ],
            cwd=ROOT,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            pass_fds=(child.fileno(),),
        )
        child.close()
        super().__init__(proc, parent_fd)
        self._read_ready()


class SerialTransport:
    def __init__(self, device: str, baudrate: int = 57600) -> None:
        try:
            import serial  # type: ignore[import-not-found]
        except ImportError as exc:
            raise RuntimeError("pyserial is required for --serial transport") from exc
        self.serial = serial.Serial(
            device,
            baudrate=baudrate,
            bytesize=serial.EIGHTBITS,
            parity=serial.PARITY_EVEN,
            stopbits=serial.STOPBITS_ONE,
            timeout=0,
            write_timeout=1,
        )

    def write(self, data: bytes) -> None:
        self.serial.write(data)
        self.serial.flush()

    def read_available(self, timeout: float) -> bytes:
        readable, _, _ = select.select([self.serial.fileno()], [], [], max(timeout, 0.0))
        if not readable:
            return b""
        return self.serial.read(4096)

    def command(self, line: str) -> str:
        raise RuntimeError(f"serial transport cannot handle local responder command {line!r}")

    def close(self) -> None:
        self.serial.close()
