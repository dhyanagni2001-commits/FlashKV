#!/usr/bin/env python3

import socket
import subprocess
import tempfile
import time
from pathlib import Path


HOST = "127.0.0.1"
PORT = 6379

PROJECT_ROOT = Path(__file__).resolve().parent.parent
SERVER_BINARY = PROJECT_ROOT / "build" / "flashkv_server"


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

    def read_response(self):
        prefix = self.reader.read(1)

        if not prefix:
            raise RuntimeError("Server closed the connection")

        if prefix == b"+":
            return self.read_line()

        if prefix == b"-":
            error = self.read_line()
            raise RuntimeError(f"Redis error: {error}")

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


def start_server(working_directory):
    process = subprocess.Popen(
        [str(SERVER_BINARY)],
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
    finally:
        client.close()


def main():
    if not SERVER_BINARY.exists():
        raise RuntimeError(
            "Server binary was not found. Run:\n"
            "cmake --build build"
        )

    if port_is_in_use():
        raise RuntimeError(
            "Port 6379 is already in use. "
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