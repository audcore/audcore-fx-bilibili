"""Check the packaged rack ABI without opening an editor window."""

import ctypes as c
import json
from pathlib import Path
import subprocess
import sys
from urllib.parse import urlencode
from urllib.request import Request, urlopen

u32 = c.c_uint32
handle = c.c_void_p
floats = c.POINTER(c.c_float)
bytes_ptr = c.POINTER(c.c_uint8)


class Rack(c.Structure):
    _fields_ = [
        ("size", u32), ("abi", u32), ("code", c.c_char_p), ("fuid", c.c_char_p),
        ("version", c.c_char_p), ("create", c.CFUNCTYPE(handle)),
        ("destroy", c.CFUNCTYPE(None, handle)),
        ("prepare", c.CFUNCTYPE(c.c_int, handle, u32, u32, u32, c.POINTER(u32))),
        ("process", c.CFUNCTYPE(None, handle, floats, floats, u32, c.c_double)),
        ("get", c.CFUNCTYPE(c.c_float, handle, u32)),
        ("set", c.CFUNCTYPE(c.c_int, handle, u32, c.c_float)),
        ("save", c.CFUNCTYPE(u32, handle, bytes_ptr, u32)),
        ("restore", c.CFUNCTYPE(c.c_int, handle, bytes_ptr, u32)),
        ("visual_active", c.CFUNCTYPE(None, handle, c.c_int)),
        ("visual", c.CFUNCTYPE(u32, handle, floats, u32)),
    ]


def main(path):
    library = c.CDLL(str(Path(path).resolve()))
    library.audcore_fx_get_rack.argtypes = [u32]
    library.audcore_fx_get_rack.restype = c.POINTER(Rack)
    assert not library.audcore_fx_get_rack(1)
    rack = library.audcore_fx_get_rack(2).contents
    assert rack.code == b"BILIBILI" and rack.size == c.sizeof(Rack)
    library.audcore_fx_get_definition.argtypes = [u32, c.POINTER(u32)]
    library.audcore_fx_get_definition.restype = handle
    count = u32()
    definition = library.audcore_fx_get_definition(1, c.byref(count))
    definition = json.loads(c.string_at(definition, count.value))
    assert definition["label"]["zh-CN"] == "\u54d4\u54e9\u54d4\u54e9"
    assert definition["fuid"] == rack.fuid.decode()
    library.audcore_fx_get_editor.restype = handle
    assert library.audcore_fx_get_editor(1)
    instances = [rack.create(), rack.create()]
    assert all(instances)
    try:
        ids = []
        for instance in instances:
            latency = u32(100)
            assert rack.prepare(instance, 48000, 128, 2, c.byref(latency))
            assert latency.value == 0
            samples = (c.c_float * 256)(*([0.25, -0.5] * 128))
            rack.process(instance, samples, None, 128, 0)
            assert list(samples) == [0.25, -0.5] * 128
            values = (c.c_float * 3)()
            assert rack.visual(instance, values, len(values)) == 3
            assert values[2] == 48000
            ids.append(values[:2])
        assert ids[0] != ids[1]
        action = library.audcore_fx_rack_action
        action.argtypes = [handle, c.c_char_p]
        action.restype = c.c_int
        get_text = library.audcore_fx_rack_get_text
        get_text.argtypes = [handle, c.c_char_p, c.c_char_p, u32]
        get_text.restype = u32
        assert not action(instances[0], b"video:invalid")
        assert action(instances[0], b"video:BV1F8yVzKETm:2")
        assert rack.set(instances[0], 0, 1)
        state = (c.c_uint8 * rack.save(instances[0], None, 0))()
        assert rack.save(instances[0], state, len(state)) == len(state)
        assert rack.restore(instances[1], state, len(state))
        assert rack.get(instances[1], 0) == 1
        video_id = c.create_string_buffer(22)
        assert get_text(instances[1], b"video_id", video_id, len(video_id))
        assert video_id.value == b"BV1F8yVzKETm"
        assert get_text(instances[1], b"video_part", video_id, len(video_id))
        assert video_id.value == b"2"
        legacy_id = b"BV1YfJJzVEDF"
        legacy_state = (c.c_uint8 * (2 + len(legacy_id))).from_buffer_copy(bytes([2, 0]) + legacy_id)
        assert rack.restore(instances[1], legacy_state, len(legacy_state))
        assert get_text(instances[1], b"video_part", video_id, len(video_id))
        assert video_id.value == b"1"
    finally:
        for instance in instances:
            rack.destroy(instance)
    print("BILIBILI rack ABI, editor, instance isolation, bypass and state verified")


def server_test(executable):
    subprocess.run([executable], check=True, timeout=15)
    with subprocess.Popen([executable, "--serve"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True) as server:
        try:
            base = server.stdout.readline().strip()
            assert base.startswith("http://127.0.0.1:")

            def fetch(path, parameters=None, headers=None):
                query = "?" + urlencode(parameters) if parameters else ""
                return urlopen(Request(base + path + query, headers=headers or {}), timeout=12)

            with fetch("") as response:
                page = response.read()
                assert b'id="player"' in page and b'data-video="BV1YfJJzVEDF"' in page
                assert b'id="fullscreen"' not in page
                assert all(f'class="icon icon-{icon}"'.encode() in page for icon in ("back", "danmaku", "play"))
                assert response.headers["Cross-Origin-Opener-Policy"] == "same-origin"
            with fetch("panel.css") as response:
                css = response.read()
                assert b"aspect-ratio: 4 / 3" in css and b"#player:hover" in css and b"#201a21bf" in css
                assert b"transition:" not in css
            with fetch("api/search", {"keyword": "\u97f3\u4e50\u73b0\u573a", "page": 1}) as response:
                result = json.load(response)
                assert result["code"] == 0
                cover = next(row["pic"] for row in result["data"]["result"] if row["type"] == "video")
            cover = ("https:" + cover) if cover.startswith("//") else cover.replace("http:", "https:", 1)
            with fetch("api/cover", {"url": cover}) as response:
                assert response.headers["Content-Type"] in ("image/jpeg", "image/png", "image/webp", "image/gif")
                assert len(response.read()) > 1024
            multi = "BV1F8yVzKETm"
            with fetch("api/view", {"bvid": multi}) as response:
                pages = json.load(response)["data"]["pages"]
                second = next(page for page in pages if page["page"] == 2)
            with fetch("api/play", {"bvid": multi, "cid": second["cid"]}) as response:
                second_media = json.load(response)["data"]["durl"][0]["url"]
            with fetch("api/media.mp4", {"url": second_media}, {"Range": "bytes=0-63"}) as response:
                assert response.status == 206 and len(response.read()) == 64
            bvid = "BV1YfJJzVEDF"
            with fetch("api/view", {"bvid": bvid}) as response:
                cid = json.load(response)["data"]["cid"]
            with fetch("api/dm", {"oid": cid}) as response:
                assert b"<d p=" in response.read()
            with fetch("api/play", {"bvid": bvid, "cid": cid}) as response:
                media = json.load(response)["data"]["durl"][0]["url"]
            with fetch("api/media.mp4", {"url": media}, {"Range": "bytes=0-63"}) as response:
                assert response.status == 206 and len(response.read()) == 64
            subprocess.run([executable, "--decode", base + "api/media.mp4?" + urlencode({"url": media})],
                           check=True, timeout=35)
            print("BILIBILI native loopback search, video details, danmaku, covers, part 2 and MP4 ranges verified")
        finally:
            server.stdin.write("\n")
            server.stdin.flush()
            server.wait(timeout=15)


if __name__ == "__main__":
    main(sys.argv[1])
    if len(sys.argv) > 2:
        server_test(sys.argv[2])
