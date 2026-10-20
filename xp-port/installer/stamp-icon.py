"""Put a classic .ico into an existing PE as its application icon."""
import ctypes
import struct
import sys
from ctypes import wintypes

kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
BeginUpdateResourceW = kernel32.BeginUpdateResourceW
BeginUpdateResourceW.argtypes = [wintypes.LPCWSTR, wintypes.BOOL]
BeginUpdateResourceW.restype = wintypes.HANDLE
UpdateResourceW = kernel32.UpdateResourceW
UpdateResourceW.argtypes = [
    wintypes.HANDLE,
    ctypes.c_void_p,
    ctypes.c_void_p,
    wintypes.WORD,
    wintypes.LPVOID,
    wintypes.DWORD,
]
UpdateResourceW.restype = wintypes.BOOL
EndUpdateResourceW = kernel32.EndUpdateResourceW
EndUpdateResourceW.argtypes = [wintypes.HANDLE, wintypes.BOOL]
EndUpdateResourceW.restype = wintypes.BOOL

RT_ICON = 3
RT_GROUP_ICON = 14


def stamp(exe_path, ico_path):
    data = open(ico_path, "rb").read()
    reserved, kind, count = struct.unpack_from("<HHH", data, 0)
    if reserved != 0 or kind != 1 or count < 1:
        raise SystemExit("not a classic icon file")
    images = []
    group = bytearray(struct.pack("<HHH", 0, 1, count))
    for index in range(count):
        width, height, colors, reserved_b, planes, bitcount, size, offset = struct.unpack_from(
            "<BBBBHHII", data, 6 + 16 * index
        )
        blob = data[offset : offset + size]
        if len(blob) != size or blob[:4] != b"\x28\x00\x00\x00":
            raise SystemExit("icon image %d is not a BMP icon" % (index + 1))
        images.append(blob)
        group += struct.pack(
            "<BBBBHHIH",
            width,
            height,
            colors,
            reserved_b,
            planes,
            bitcount,
            size,
            index + 1,
        )
    handle = BeginUpdateResourceW(exe_path, False)
    if not handle:
        raise SystemExit("BeginUpdateResource failed: %s" % ctypes.get_last_error())
    try:
        for index, blob in enumerate(images, start=1):
            buf = ctypes.create_string_buffer(blob)
            if not UpdateResourceW(handle, RT_ICON, index, 0, buf, len(blob)):
                raise SystemExit("UpdateResource icon %d failed: %s" % (index, ctypes.get_last_error()))
        group_buf = ctypes.create_string_buffer(bytes(group))
        if not UpdateResourceW(handle, RT_GROUP_ICON, 1, 0, group_buf, len(group)):
            raise SystemExit("UpdateResource group failed: %s" % ctypes.get_last_error())
    except Exception:
        EndUpdateResourceW(handle, True)
        raise
    if not EndUpdateResourceW(handle, False):
        raise SystemExit("EndUpdateResource failed: %s" % ctypes.get_last_error())
    print("stamped %d images into %s" % (count, exe_path))


if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit("usage: stamp-icon.py <exe> <ico>")
    stamp(sys.argv[1], sys.argv[2])
