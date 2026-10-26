"""Fetch pinned third-party sources and tools for the Xbox UWP build into xbox/.local/deps.

Every upstream archive is verified against its pinned SHA-256 before extraction. If MSYS2's rolling
mirror rotates the pinned winpthreads package revision, falls back to the local C:\\msys64\\ucrt64
installation installed by pacman in CI.
"""
import argparse
import hashlib
import io
import os
import pathlib
import shutil
import subprocess
import sys
import tarfile
import tempfile
import urllib.error
import urllib.request
import zipfile

ROOT = pathlib.Path(__file__).resolve().parents[1] / '.local' / 'deps'
MSYS2 = pathlib.Path(os.environ.get('MSYS2_ROOT', r'C:\msys64'))
GITHUB = 'https://codeload.github.com/{}/tar.gz/{}'
MSYS2_PACKAGES = 'https://repo.msys2.org/mingw/ucrt64/'
WINPTHREAD = '14.0.0.r495.g63ebe6cdb-1'

SOURCES = {
    'fmt': (
        GITHUB.format('fmtlib/fmt', '11ddbcb7898d2d3445d431a54814367b21dee6ad'),
        '7465e374dc24a8f01dde5618dcb04d66692bec9190371fd8ea4349f409867e10',
        'LICENSE',
    ),
    'sdl': (
        GITHUB.format('libsdl-org/SDL', 'fa2c02bb6e21974a89ea9824bc53c9932abe5f9c'),
        'd246967187a4f37cc850608dbaf5cff9d1df4d731259903d25539755cdfcc657',
        'LICENSE.txt',
    ),
    'zydis': (
        GITHUB.format('zyantific/zydis', '120e0e705f8e3b507dc49377ac2879979f0d545c'),
        '2ed57a3bd77a2a10fe354638ea5a487582bb944b0d897b58c0e4d2acf466c126',
        'LICENSE',
    ),
    'zycore': (
        GITHUB.format('zyantific/zycore-c', '38d4f0285e6157ee840ea82a9b90aba71c8a705d'),
        'fcc8de568292cf8f1793bf62e9fcf12bc70ea661a54c932690607f5ac2a8cb5c',
        'LICENSE',
    ),
    'xxhash': (
        GITHUB.format('Cyan4973/xxHash', 'e573d4d2aaeaba0f3e5a0a9a54144a1f2b4b56e7'),
        '714198f8441f56eeaec0f287c2a846ee2f07c646e9d8e40cf2bb8b2e759d2830',
        'LICENSE',
    ),
    'vma': (
        GITHUB.format('GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator', 'a1d434708c217b2a6c7b365f1fe41fa03a562e59'),
        '850406caac450abd5eb4d9988d4522958723796ddc10f5e0fcb5568237e9dcd' if False else '850406caac450abd5eb4d9988d45229587232f96ddc10f5e0fcb5568237e9dcd',
        'LICENSE.txt',
    ),
    'vulkan-headers': (
        GITHUB.format('KhronosGroup/Vulkan-Headers', 'refs/tags/v1.4.365'),
        'ed832b3292cce324ebba6522fd2d3004bf1b797fa49bfbcad0b302ff23d56abc',
        'LICENSE.md',
    ),
    'robin-map': (
        GITHUB.format('Tessil/robin-map', 'refs/tags/v1.3.0'),
        'a8424ad3b0affd4c57ed26f0f3d8a29604f0e1f2ef2089f497f614b1c94c7236',
        'LICENSE',
    ),
    'glslang': (
        GITHUB.format('KhronosGroup/glslang', 'refs/tags/16.6.0'),
        '9c09b901149c729df745057dafa815278aaa101b84d2b6e14f16a42de52f97f2',
        'LICENSE.txt',
    ),
    'ffmpeg': (
        'https://ffmpeg.org/releases/ffmpeg-7.1.1.tar.xz',
        '733984395e0dbbe5c046abda2dc49a5544e7e0e1e2366bba849222ae9e3a03b1',
        'COPYING.LGPLv2.1',
    ),
    'nasm': (
        'https://www.nasm.us/pub/nasm/releasebuilds/2.16.03/win64/nasm-2.16.03-win64.zip',
        '3ee4782247bcb874378d02f7eab4e294a84d3d15f3f6ee2de2f47a46aa7226e6',
        'LICENSE',
    ),
    'libwinpthread': (
        MSYS2_PACKAGES + f'mingw-w64-ucrt-x86_64-libwinpthread-{WINPTHREAD}-any.pkg.tar.zst',
        '1f847f4f5d56d8f54cbc9b5ce339c36f4f208353e813d4b1e5648cf33138b9a2',
        None,
    ),
    'winpthreads': (
        MSYS2_PACKAGES + f'mingw-w64-ucrt-x86_64-winpthreads-{WINPTHREAD}-any.pkg.tar.zst',
        'd7d9e9dc493f2096fb441da8d6f5cb39cf90475d4b02e4808d73e3194a87ab17',
        None,
    ),
}


def download(url):
    request = urllib.request.Request(url, headers={'User-Agent': 'Bloodborne-Xbox-UWP-build'})
    with urllib.request.urlopen(request, timeout=120) as response:
        return response.read()


def fallback_msys2_winpthreads(name, target):
    ucrt = MSYS2 / 'ucrt64'
    if name == 'libwinpthread':
        dll = ucrt / 'bin' / 'libwinpthread-1.dll'
        if not dll.is_file():
            return False
        (target / 'bin').mkdir(parents=True, exist_ok=True)
        shutil.copy2(dll, target / 'bin' / 'libwinpthread-1.dll')
        return True
    if name == 'winpthreads':
        imp = ucrt / 'lib' / 'libwinpthread.dll.a'
        hdr = ucrt / 'include' / 'pthread.h'
        lic = ucrt / 'share' / 'licenses' / 'winpthreads' / 'COPYING'
        if not (imp.is_file() and hdr.is_file() and lic.is_file()):
            return False
        (target / 'include').mkdir(parents=True, exist_ok=True)
        (target / 'lib').mkdir(parents=True, exist_ok=True)
        (target / 'share' / 'licenses' / 'winpthreads').mkdir(parents=True, exist_ok=True)
        for pattern in ('pthread*.h', 'sched.h', 'semaphore.h'):
            for f in (ucrt / 'include').glob(pattern):
                shutil.copy2(f, target / 'include' / f.name)
        shutil.copy2(imp, target / 'lib' / 'libwinpthread.dll.a')
        shutil.copy2(lic, target / 'share' / 'licenses' / 'winpthreads' / 'COPYING')
        return True
    return False


def unpack(name, url, data, target):
    temporary = pathlib.Path(tempfile.mkdtemp(prefix=name + '-', dir=ROOT))
    try:
        if url.endswith('.zip'):
            with zipfile.ZipFile(io.BytesIO(data)) as archive:
                archive.extractall(temporary)
        elif url.endswith('.pkg.tar.zst'):
            package = temporary / 'package.tar.zst'
            package.write_bytes(data)
            subprocess.run(
                [str(MSYS2 / 'usr/bin/zstd.exe'), '-q', '-d', str(package), '-o',
                 str(temporary / 'package.tar')],
                check=True,
            )
            with tarfile.open(temporary / 'package.tar') as archive:
                archive.extractall(temporary)
            package.unlink()
            (temporary / 'package.tar').unlink()
        else:
            with tarfile.open(fileobj=io.BytesIO(data), mode='r:*') as archive:
                archive.extractall(temporary)
        entries = [entry for entry in temporary.iterdir() if not entry.name.startswith('.')]
        source = entries[0] if len(entries) == 1 and entries[0].is_dir() else temporary
        shutil.move(str(source), str(target))
    finally:
        shutil.rmtree(temporary, ignore_errors=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--hashes', action='store_true', help='print archive SHA-256s and stop')
    args = parser.parse_args()
    ROOT.mkdir(parents=True, exist_ok=True)
    licenses = ROOT / 'licenses'
    licenses.mkdir(exist_ok=True)
    failed = False
    for name, (url, pinned, license_file) in SOURCES.items():
        target = ROOT / name
        if target.exists() and not args.hashes:
            continue
        print(f'Fetching {name}: {url}', flush=True)
        try:
            data = download(url)
        except urllib.error.HTTPError as err:
            if not args.hashes and name in ('libwinpthread', 'winpthreads') and fallback_msys2_winpthreads(name, target):
                print(f'  Used local MSYS2 UCRT64 {name} fallback after HTTP {err.code}', flush=True)
                continue
            raise
        digest = hashlib.sha256(data).hexdigest()
        if args.hashes:
            print(f'  {name}: {digest} ({len(data) >> 10} KiB)')
            continue
        if digest != pinned:
            print(f'  SHA-256 mismatch for {name}: got {digest}, pinned {pinned}', file=sys.stderr)
            failed = True
            continue
        unpack(name, url, data, target)
        if license_file:
            shutil.copyfile(target / license_file, licenses / f'{name}.txt')
    if failed:
        sys.exit(1)
    if not args.hashes:
        print(f'Dependencies ready in {ROOT}')


if __name__ == '__main__':
    main()
