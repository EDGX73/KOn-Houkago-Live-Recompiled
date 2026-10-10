"""Copies every resource of a compiled .res file (icon, version information)
into an executable with the Windows resource API, after linking (zig c++
does not link .res files).

Usage: python tools/embed_resources.py app.res program.exe
"""
import ctypes, struct, sys
from ctypes import wintypes

def read_id(data, at):
    """A resource type or name: 0xFFFF + ordinal, or a NUL-terminated UTF-16 string."""
    if struct.unpack_from('<H', data, at)[0] == 0xFFFF:
        return struct.unpack_from('<H', data, at + 2)[0], at + 4
    end = at
    while struct.unpack_from('<H', data, end)[0]:
        end += 2
    return data[at:end].decode('utf-16-le'), end + 2

def entries(data):
    at = 0
    while at + 8 <= len(data):
        size, header = struct.unpack_from('<II', data, at)
        kind, p = read_id(data, at + 8)
        name, p = read_id(data, p)
        p = (p + 3) & ~3
        _, _, language, _, _ = struct.unpack_from('<IHHII', data, p)
        body = data[at + header:at + header + size]
        if not (kind == 0 and name == 0):
            yield kind, name, language, body
        at = (at + header + size + 3) & ~3

def main():
    res, exe = sys.argv[1], sys.argv[2]
    data = open(res, 'rb').read()
    k = ctypes.WinDLL('kernel32', use_last_error=True)
    k.BeginUpdateResourceW.restype = wintypes.HANDLE
    k.BeginUpdateResourceW.argtypes = [wintypes.LPCWSTR, wintypes.BOOL]
    k.UpdateResourceW.argtypes = [wintypes.HANDLE, ctypes.c_void_p, ctypes.c_void_p, wintypes.WORD, ctypes.c_void_p, wintypes.DWORD]
    k.EndUpdateResourceW.argtypes = [wintypes.HANDLE, wintypes.BOOL]
    handle = k.BeginUpdateResourceW(exe, False)
    if not handle:
        raise SystemExit(f'BeginUpdateResource failed ({ctypes.get_last_error()})')
    count = 0
    for kind, name, language, body in entries(data):
        as_ptr = lambda v: ctypes.c_void_p(v) if isinstance(v, int) else ctypes.c_wchar_p(v)
        buffer = ctypes.create_string_buffer(body, len(body))
        if not k.UpdateResourceW(handle, as_ptr(kind), as_ptr(name), language, buffer, len(body)):
            raise SystemExit(f'UpdateResource failed ({ctypes.get_last_error()})')
        count += 1
    if not k.EndUpdateResourceW(handle, False):
        raise SystemExit(f'EndUpdateResource failed ({ctypes.get_last_error()})')
    print(f'{count} resources embedded in {exe}')

if __name__ == '__main__':
    main()
