"""RNDISStorage Windows CLI — HTTP/WebDAV filesystem PoC (no USB MSC)."""

from __future__ import annotations

import argparse
import json
import logging
import sys

import requests

from rndisstorage import (
    DEFAULT_BASE_URL,
    DEFAULT_TIMEOUT,
    RndisStorageClient,
    RndisStorageError,
    discover_default_urls,
    normalize_base_url,
)

LOG = logging.getLogger("rndisstorage.cli")


def _with_client(args: argparse.Namespace) -> RndisStorageClient:
    return RndisStorageClient(normalize_base_url(args.url), timeout=args.timeout)


def cmd_discover(_args: argparse.Namespace) -> int:
    print("Suggested device URLs (join SoftAP SSID RNDISStorage first, or use USB RNDIS IP):")
    for u in discover_default_urls():
        print(f"  {u}")
    return 0


def cmd_info(args: argparse.Namespace) -> int:
    with _with_client(args) as c:
        print(json.dumps(c.info(), indent=2))
        print(json.dumps(c.storage(), indent=2))
    return 0


def cmd_ls(args: argparse.Namespace) -> int:
    with _with_client(args) as c:
        print(json.dumps(c.ls(args.path), indent=2))
    return 0


def cmd_stat(args: argparse.Namespace) -> int:
    with _with_client(args) as c:
        print(json.dumps(c.stat(args.path), indent=2))
    return 0


def cmd_get(args: argparse.Namespace) -> int:
    with _with_client(args) as c:
        print(json.dumps(c.get(args.remote, args.local), indent=2))
    return 0


def cmd_put(args: argparse.Namespace) -> int:
    with _with_client(args) as c:
        print(json.dumps(c.put(args.local, args.remote), indent=2))
    return 0


def cmd_delete(args: argparse.Namespace) -> int:
    with _with_client(args) as c:
        print(json.dumps(c.delete(args.path), indent=2))
    return 0


def cmd_mkdir(args: argparse.Namespace) -> int:
    with _with_client(args) as c:
        print(json.dumps(c.mkdir(args.path), indent=2))
    return 0


def cmd_rename(args: argparse.Namespace) -> int:
    with _with_client(args) as c:
        print(json.dumps(c.rename(args.path, args.new_path), indent=2))
    return 0


def cmd_touch(args: argparse.Namespace) -> int:
    with _with_client(args) as c:
        print(json.dumps(c.touch(args.path), indent=2))
    return 0


def cmd_format(args: argparse.Namespace) -> int:
    with _with_client(args) as c:
        print(json.dumps(c.format_storage(args.filesystem, confirm=args.confirm), indent=2))
    return 0


def cmd_propfind(args: argparse.Namespace) -> int:
    with _with_client(args) as c:
        print(json.dumps(c.propfind(args.path), indent=2))
    return 0


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(description="RNDISStorage WebDAV/HTTP client")
    p.add_argument("-v", "--verbose", action="store_true")
    p.add_argument(
        "--url",
        default=DEFAULT_BASE_URL,
        help=f"Device base URL (default {DEFAULT_BASE_URL})",
    )
    p.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT)
    sub = p.add_subparsers(dest="cmd", required=True)

    sub.add_parser("discover", help="Print suggested device URLs").set_defaults(func=cmd_discover)

    sp = sub.add_parser("info")
    sp.set_defaults(func=cmd_info)

    sp = sub.add_parser("ls")
    sp.add_argument("path", nargs="?", default="/")
    sp.set_defaults(func=cmd_ls)

    sp = sub.add_parser("stat")
    sp.add_argument("path")
    sp.set_defaults(func=cmd_stat)

    sp = sub.add_parser("get")
    sp.add_argument("remote")
    sp.add_argument("local")
    sp.set_defaults(func=cmd_get)

    sp = sub.add_parser("put")
    sp.add_argument("local")
    sp.add_argument("remote")
    sp.set_defaults(func=cmd_put)

    sp = sub.add_parser("delete")
    sp.add_argument("path")
    sp.set_defaults(func=cmd_delete)

    sp = sub.add_parser("mkdir")
    sp.add_argument("path")
    sp.set_defaults(func=cmd_mkdir)

    sp = sub.add_parser("rename")
    sp.add_argument("path")
    sp.add_argument("new_path")
    sp.set_defaults(func=cmd_rename)

    sp = sub.add_parser("touch")
    sp.add_argument("path")
    sp.set_defaults(func=cmd_touch)

    sp = sub.add_parser("format")
    sp.add_argument("--filesystem", default="fat32")
    sp.add_argument("--confirm", required=True)
    sp.set_defaults(func=cmd_format)

    sp = sub.add_parser("propfind", help="Raw WebDAV PROPFIND")
    sp.add_argument("path", nargs="?", default="/")
    sp.set_defaults(func=cmd_propfind)

    return p


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    logging.basicConfig(
        level=logging.DEBUG if args.verbose else logging.INFO,
        format="%(levelname)s %(name)s: %(message)s",
    )
    try:
        return int(args.func(args))
    except RndisStorageError as exc:
        LOG.error("%s", exc)
        return 2
    except requests.RequestException as exc:
        LOG.error("HTTP error: %s", exc)
        return 2


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except KeyboardInterrupt:
        raise SystemExit(130)
