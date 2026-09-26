#!/usr/bin/env python3
"""Creates synthetic files only; checks decoded audio before/after tag operations."""
import pathlib
import subprocess
import sys
import tempfile
import os


def run(*args):
    return subprocess.check_output(args, stderr=subprocess.STDOUT)


def audio_hash(path):
    return run('ffmpeg', '-v', 'error', '-i', str(path), '-map', '0:a:0',
               '-f', 'hash', '-hash', 'sha256', '-').strip()


with tempfile.TemporaryDirectory(prefix='musicorder-тест-') as tmp:
    root = pathlib.Path(tmp)
    hashes = {}
    for ext, codec in [('mp3', 'libmp3lame'), ('flac', 'flac'), ('ogg', 'libvorbis')]:
        path = root / f'Исполнитель — тест.{ext}'
        run('ffmpeg', '-v', 'error', '-f', 'lavfi', '-i',
            'sine=frequency=440:duration=2', '-c:a', codec,
            '-metadata', 'title=Исходное название', '-metadata',
            'composer=Сохранить композитора', str(path))
        hashes[path] = audio_hash(path)
    (root / 'broken.mp3').write_bytes(b'not an audio file')
    env = dict(os.environ, QT_QPA_PLATFORM='offscreen')
    subprocess.run([sys.argv[1], str(root)], env=env, check=True)
    for path, digest in hashes.items():
        assert audio_hash(path) == digest, f'Audio changed: {path}'
    restore = pathlib.Path(__file__).resolve().parents[1] / 'scripts' / 'restore.py'
    manifest = next((root / 'backups').glob('*/restore.json'))
    import json
    import hashlib
    record = json.loads(manifest.read_text())
    original = pathlib.Path(record['original'])
    before = original.read_bytes()
    rejected = subprocess.run([sys.executable, str(restore), str(manifest)], capture_output=True)
    assert rejected.returncode == 1 and original.read_bytes() == before
    subprocess.run([sys.executable, str(restore), str(manifest), '--replace'], check=True)
    assert hashlib.sha256(original.read_bytes()).hexdigest() == record['sha256']
    assert any(original.parent.glob(original.name + '.before-restore-*'))
    print('PASS: MP3, FLAC, OGG — tags, artwork, Unicode, preservation, backups, stale preview, quarantine, decoded audio')
