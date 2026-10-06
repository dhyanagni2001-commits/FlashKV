#!/usr/bin/env python3
"""End-to-end tests that drive a real flashkv_server over TCP."""

import argparse
import socket
import subprocess
import tempfile
import threading
import time
from pathlib import Path


HOST = "127.0.0.1"
PORT = 6379

PROJECT_ROOT = Path(__file__).resolve().parent.parent
SERVER_BINARY = PROJECT_ROOT / "build" / "flashkv_server"


class RedisError(Exception):
    pass


class RespClient:
    def __init__(self):
        self.socket = socket.create_connection(
            (HOST, PORT),
            timeout=2,
        )
        self.reader = self.socket.makefile("rb")

    def close(self):
        self.reader.close()
        self.socket.close()

    def command(self, *arguments):
        encoded_arguments = [
            str(argument).encode("utf-8")
            for argument in arguments
        ]

        request = (
            f"*{len(encoded_arguments)}\r\n".encode()
        )

        for argument in encoded_arguments:
            request += (
                f"${len(argument)}\r\n".encode()
                + argument
                + b"\r\n"
            )

        self.socket.sendall(request)
        return self.read_response()

    def command_or_error(self, *arguments):
        try:
            return self.command(*arguments)
        except RedisError as error:
            return error

    def read_response(self):
        prefix = self.reader.read(1)

        if not prefix:
            raise RuntimeError("Server closed the connection")

        if prefix == b"+":
            return self.read_line()

        if prefix == b"-":
            raise RedisError(self.read_line())

        if prefix == b":":
            return int(self.read_line())

        if prefix == b"$":
            length = int(self.read_line())

            if length == -1:
                return None

            value = self.reader.read(length)
            ending = self.reader.read(2)

            if ending != b"\r\n":
                raise RuntimeError(
                    "Invalid bulk-string ending"
                )

            return value.decode("utf-8")

        if prefix == b"*":
            count = int(self.read_line())

            if count == -1:
                return None

            return [self.read_response() for _ in range(count)]

        raise RuntimeError(
            f"Unknown RESP prefix: {prefix!r}"
        )

    def read_line(self):
        line = self.reader.readline()

        if not line.endswith(b"\r\n"):
            raise RuntimeError("Invalid RESP line")

        return line[:-2].decode("utf-8")


def assert_equal(actual, expected, description):
    if actual != expected:
        raise AssertionError(
            f"{description}: expected {expected!r}, "
            f"received {actual!r}"
        )

    print(f"PASS: {description}")


def port_is_in_use():
    with socket.socket(
        socket.AF_INET,
        socket.SOCK_STREAM,
    ) as check_socket:
        check_socket.settimeout(0.2)

        return (
            check_socket.connect_ex(
                (HOST, PORT)
            )
            == 0
        )


def find_free_port():
    with socket.socket() as probe:
        probe.bind((HOST, 0))
        return probe.getsockname()[1]


def start_server(working_directory, *extra_arguments):
    process = subprocess.Popen(
        [str(SERVER_BINARY), "--port", str(PORT), *extra_arguments],
        cwd=working_directory,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )

    deadline = time.time() + 5

    while time.time() < deadline:
        if process.poll() is not None:
            output = process.communicate()[0]

            raise RuntimeError(
                "FlashKV exited during startup:\n"
                + output
            )

        try:
            connection = socket.create_connection(
                (HOST, PORT),
                timeout=0.2,
            )
            connection.close()
            return process
        except OSError:
            time.sleep(0.05)

    stop_server(process)

    raise RuntimeError(
        "Timed out waiting for FlashKV to start"
    )


def stop_server(process):
    if process.poll() is not None:
        return

    process.terminate()

    try:
        process.wait(timeout=3)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=3)


def test_basic_commands():
    client = RespClient()

    try:
        assert_equal(
            client.command("PING"),
            "PONG",
            "PING",
        )

        assert_equal(
            client.command("ECHO", "hello"),
            "hello",
            "ECHO",
        )

        assert_equal(
            client.command("SET", "name", "Dhyan"),
            "OK",
            "SET",
        )

        assert_equal(
            client.command("GET", "name"),
            "Dhyan",
            "GET",
        )

        assert_equal(
            client.command("EXISTS", "name"),
            1,
            "EXISTS existing key",
        )

        assert_equal(
            client.command("DEL", "name"),
            1,
            "DEL",
        )

        assert_equal(
            client.command("GET", "name"),
            None,
            "GET missing key",
        )
    finally:
        client.close()


def test_expiration():
    client = RespClient()

    try:
        assert_equal(
            client.command(
                "SET",
                "temporary",
                "hello",
            ),
            "OK",
            "SET expiring key",
        )

        assert_equal(
            client.command(
                "EXPIRE",
                "temporary",
                1,
            ),
            1,
            "EXPIRE",
        )

        ttl = client.command("TTL", "temporary")

        if ttl not in (0, 1):
            raise AssertionError(
                f"TTL expected 0 or 1, received {ttl}"
            )

        print("PASS: TTL countdown")

        time.sleep(1.2)

        assert_equal(
            client.command("GET", "temporary"),
            None,
            "Expired key removal",
        )

        assert_equal(
            client.command("TTL", "temporary"),
            -2,
            "TTL missing key",
        )
    finally:
        client.close()


def test_multiple_clients():
    first_client = RespClient()
    second_client = RespClient()

    try:
        assert_equal(
            first_client.command(
                "SET",
                "client-one",
                "first",
            ),
            "OK",
            "Client 1 SET",
        )

        assert_equal(
            second_client.command(
                "GET",
                "client-one",
            ),
            "first",
            "Client 2 reads Client 1 value",
        )

        assert_equal(
            second_client.command(
                "SET",
                "client-two",
                "second",
            ),
            "OK",
            "Client 2 SET",
        )

        assert_equal(
            first_client.command(
                "GET",
                "client-two",
            ),
            "second",
            "Client 1 reads Client 2 value",
        )
    finally:
        first_client.close()
        second_client.close()


def test_data_types():
    client = RespClient()

    try:
        assert_equal(
            client.command("RPUSH", "queue", "a", "b", "c"),
            3,
            "RPUSH",
        )
        assert_equal(
            client.command("LPUSH", "queue", "start"),
            4,
            "LPUSH",
        )
        assert_equal(
            client.command("LRANGE", "queue", 0, -1),
            ["start", "a", "b", "c"],
            "LRANGE",
        )
        assert_equal(
            client.command("RPOP", "queue"),
            "c",
            "RPOP",
        )
        assert_equal(
            client.command("HSET", "user:1", "name", "Ada", "lang", "C++"),
            2,
            "HSET",
        )
        assert_equal(
            client.command("HGETALL", "user:1"),
            ["lang", "C++", "name", "Ada"],
            "HGETALL",
        )
        assert_equal(
            client.command("SADD", "tags", "fast", "cpp", "fast"),
            2,
            "SADD",
        )
        assert_equal(
            client.command("SMEMBERS", "tags"),
            ["cpp", "fast"],
            "SMEMBERS",
        )
        assert_equal(
            client.command("INCRBY", "visits", 10),
            10,
            "INCRBY",
        )
        assert_equal(
            client.command("TYPE", "user:1"),
            "hash",
            "TYPE",
        )

        error = client.command_or_error("GET", "queue")

        if not isinstance(error, RedisError) or not str(error).startswith(
            "WRONGTYPE"
        ):
            raise AssertionError(f"Expected WRONGTYPE, received {error!r}")

        print("PASS: WRONGTYPE error")
    finally:
        client.close()


def test_inline_and_partial_commands():
    connection = socket.create_connection((HOST, PORT), timeout=2)

    try:
        connection.sendall(b'SET inline "hello world"\r\nGET inline\r\n')
        expected = b"+OK\r\n$11\r\nhello world\r\n"
        received = b""

        while len(received) < len(expected):
            received += connection.recv(1024)

        assert_equal(received, expected, "Inline commands")

        # Deliver a RESP command one byte at a time.
        for byte in b"*2\r\n$4\r\nECHO\r\n$5\r\nbytes\r\n":
            connection.sendall(bytes([byte]))
            time.sleep(0.001)

        assert_equal(
            connection.recv(1024),
            b"$5\r\nbytes\r\n",
            "Byte-by-byte partial command",
        )
    finally:
        connection.close()


def test_large_pipeline():
    """Replies larger than the socket buffer exercise POLLOUT writes."""
    client = RespClient()
    value = "v" * 1024
    count = 5000

    try:
        client.command("SET", "big", value)

        request = (
            f"*2\r\n$3\r\nGET\r\n$3\r\nbig\r\n".encode() * count
        )

        # Send from another thread while reading, like a real client.
        sender = threading.Thread(
            target=client.socket.sendall,
            args=(request,),
        )
        sender.start()

        for _ in range(count):
            if client.read_response() != value:
                raise AssertionError("Pipelined reply mismatch")

        sender.join()
        print(f"PASS: {count} pipelined GETs ({count * len(value) // 1024} KiB)")
    finally:
        client.close()


def test_many_concurrent_clients():
    errors = []

    def worker(index):
        try:
            client = RespClient()

            for step in range(50):
                key = f"concurrent:{index}:{step}"
                client.command("SET", key, step)

                if client.command("GET", key) != str(step):
                    errors.append(key)

            client.command("INCR", "concurrent:total")
            client.close()
        except Exception as error:  # noqa: BLE001
            errors.append(repr(error))

    threads = [
        threading.Thread(target=worker, args=(index,))
        for index in range(40)
    ]

    for thread in threads:
        thread.start()

    for thread in threads:
        thread.join()

    if errors:
        raise AssertionError(f"Concurrent client errors: {errors[:3]}")

    client = RespClient()

    try:
        assert_equal(
            client.command("GET", "concurrent:total"),
            "40",
            "40 concurrent clients",
        )
    finally:
        client.close()


def info_field(client, name):
    for line in client.command("INFO").splitlines():
        if line.startswith(name + ":"):
            return line.split(":", 1)[1]

    raise AssertionError(f"INFO field {name} missing")


def test_active_expiration():
    client = RespClient()

    try:
        before = int(info_field(client, "expired_keys"))

        for index in range(200):
            client.command("SET", f"volatile:{index}", "x", "PX", 100)

        # Never touch the keys again; the background cycle must
        # reclaim them on its own.
        time.sleep(1.0)

        reclaimed = int(info_field(client, "expired_keys")) - before

        if reclaimed < 200:
            raise AssertionError(
                f"Active expiry reclaimed only {reclaimed} of 200 keys"
            )

        print("PASS: Active expiration reclaimed 200 untouched keys")
    finally:
        client.close()


def test_lru_eviction():
    client = RespClient()

    try:
        client.command("FLUSHALL")
        client.command("CONFIG", "SET", "maxmemory-policy", "allkeys-lru")
        client.command("CONFIG", "SET", "maxmemory", "64kb")

        client.command("SET", "hot", "keep me")

        for index in range(2000):
            client.command("SET", f"cold:{index}", "x" * 64)

            # Keep "hot" recently used so LRU never picks it.
            if index % 50 == 0:
                client.command("GET", "hot")

        used = int(info_field(client, "used_memory"))
        evicted = int(info_field(client, "evicted_keys"))

        if used > 64 * 1024 + 1024:
            raise AssertionError(f"used_memory {used} exceeds maxmemory")

        if evicted == 0:
            raise AssertionError("No keys were evicted")

        assert_equal(client.command("GET", "hot"), "keep me", "LRU keeps hot key")
        assert_equal(client.command("EXISTS", "cold:0"), 0, "LRU evicts cold key")

        # Like Redis, the limit is checked before each write, so push
        # usage over the limit first, then expect the next write to fail.
        client.command("CONFIG", "SET", "maxmemory-policy", "noeviction")
        client.command_or_error("SET", "filler:1", "x" * 4096)
        client.command_or_error("SET", "filler:2", "x" * 4096)
        error = client.command_or_error("SET", "rejected", "x")

        if not isinstance(error, RedisError) or not str(error).startswith("OOM"):
            raise AssertionError(f"Expected OOM, received {error!r}")

        print("PASS: noeviction returns OOM")

        client.command("CONFIG", "SET", "maxmemory", "0")
    finally:
        client.close()


def test_quit():
    client = RespClient()

    try:
        assert_equal(client.command("QUIT"), "OK", "QUIT")

        if client.reader.read(1) != b"":
            raise AssertionError("Connection still open after QUIT")
    finally:
        client.close()


def create_persistent_value():
    client = RespClient()

    try:
        assert_equal(
            client.command(
                "SET",
                "persistent",
                "survives-restart",
            ),
            "OK",
            "Create persistent value",
        )
        client.command("RPUSH", "persistent:list", "a", "b")
        client.command("HSET", "persistent:hash", "field", "value")
        client.command("SADD", "persistent:set", "member")
        client.command("SET", "persistent:ttl", "value", "EX", 100)
    finally:
        client.close()


def test_recovered_value():
    client = RespClient()

    try:
        assert_equal(
            client.command("GET", "persistent"),
            "survives-restart",
            "AOF restart recovery",
        )
        assert_equal(
            client.command("LRANGE", "persistent:list", 0, -1),
            ["a", "b"],
            "AOF restores lists",
        )
        assert_equal(
            client.command("HGET", "persistent:hash", "field"),
            "value",
            "AOF restores hashes",
        )
        assert_equal(
            client.command("SISMEMBER", "persistent:set", "member"),
            1,
            "AOF restores sets",
        )

        ttl = client.command("TTL", "persistent:ttl")

        if not 95 <= ttl <= 100:
            raise AssertionError(f"Restored TTL out of range: {ttl}")

        print("PASS: AOF restores TTLs as absolute deadlines")
    finally:
        client.close()


def main():
    global PORT, SERVER_BINARY

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server", type=Path, default=SERVER_BINARY)
    parser.add_argument("--port", type=int, default=0)
    arguments = parser.parse_args()

    SERVER_BINARY = arguments.server.resolve()
    PORT = arguments.port or find_free_port()

    if not SERVER_BINARY.exists():
        raise RuntimeError(
            "Server binary was not found. Run:\n"
            "cmake --build build"
        )

    if port_is_in_use():
        raise RuntimeError(
            f"Port {PORT} is already in use. "
            "Stop the running server first."
        )

    server = None

    with tempfile.TemporaryDirectory() as directory:
        try:
            print("Starting FlashKV...")
            server = start_server(directory)

            test_basic_commands()
            test_expiration()
            test_multiple_clients()
            test_data_types()
            test_inline_and_partial_commands()
            test_large_pipeline()
            test_many_concurrent_clients()
            test_active_expiration()
            test_lru_eviction()
            test_quit()
            create_persistent_value()

            print("Restarting FlashKV...")
            stop_server(server)
            server = start_server(directory)

            test_recovered_value()

            print("\nAll FlashKV integration tests passed!")
        finally:
            if server is not None:
                stop_server(server)


if __name__ == "__main__":
    main()