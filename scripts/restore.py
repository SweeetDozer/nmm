#!/usr/bin/env python3
"""Restore a MusicOrder backup after checking its SHA-256 digest."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import uuid


def digest(path):
    with path.open('rb') as f:
        return hashlib.file_digest(f, 'sha256').hexdigest()


def restore(manifest, replace=False):
    record = json.loads(manifest.read_text(encoding='utf-8'))
    original = Path(record['original'])
    backup = Path(record['backup'])
    expected = record['sha256']
    if not original.is_absolute() or not backup.is_absolute():
        raise ValueError('Журнал должен содержать абсолютные пути')
    if digest(backup) != expected:
        raise ValueError('SHA-256 копии не совпадает с журналом')
    if original.is_symlink():
        raise ValueError('Отказ от замены символической ссылки')
    if original.exists() and not replace:
        raise ValueError('Исходный путь занят. Для замены передайте --replace; текущий файл будет сохранён рядом.')
    original.parent.mkdir(parents=True, exist_ok=True)
    staging = original.with_name(original.name + '.restoring-' + uuid.uuid4().hex)
    try:
        shutil.copy2(backup, staging)
        if digest(staging) != expected:
            raise ValueError('Ошибка проверки восстановленной копии')
        if original.exists():
            preserved = original.with_name(original.name + '.before-restore-' + uuid.uuid4().hex)
            shutil.copy2(original, preserved)
            if digest(preserved) != digest(original):
                raise ValueError('Не удалось сохранить текущий файл')
            print(f'Текущий файл сохранён: {preserved}')
        elif not replace:
            # Reserve the destination without replacing a file created concurrently.
            with original.open('xb'):
                pass
        os.replace(staging, original)
    finally:
        staging.unlink(missing_ok=True)
    print(f'Восстановлен: {original}')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('manifest', type=Path, help='Путь к restore.json')
    parser.add_argument('--replace', action='store_true', help='Разрешить замену, сохранив текущий файл рядом')
    args = parser.parse_args()
    try:
        restore(args.manifest, args.replace)
    except (OSError, ValueError, KeyError) as exc:
        parser.exit(1, f'Восстановление не выполнено: {exc}\n')
