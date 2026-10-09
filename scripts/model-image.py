"""Pack read-only model resources into the reserved DDR range."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil

PAGE = 4096
DDR_BASE = 0x80000000
DDR_SIZE = 16 * 1024**3
FDT_SIZE = 256 * 1024

def digest(path):
    with path.open('rb') as source:
        return hashlib.file_digest(source, 'sha256').hexdigest()

def inventory(source):
    layout = json.loads((source / 'layout.json').read_text())
    files, offset = [], 0
    for name in sorted(set(layout['resources'].values())):
        path = Path(name)
        if path.is_absolute() or '..' in path.parts or not path.parts or '\n' in name or '\t' in name:
            raise ValueError(f'Invalid resource path: {name!r}')
        file = source / path
        if file.is_symlink() or not file.is_file():
            raise ValueError(f'Resource must be a regular file: {file}')
        offset = (offset + PAGE - 1) // PAGE * PAGE
        size = file.stat().st_size
        files.append({'path': name, 'offset': offset, 'size': size})
        offset += size
    if not files:
        raise ValueError('Empty model resources')
    return files, (offset + PAGE - 1) // PAGE * PAGE

def index(source, files, base, size):
    return {'version': 1, 'page_bytes': PAGE, 'model_base': base, 'size': size,
            'files': [dict(item, sha256=digest(source / item['path'])) for item in files]}

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('mode', choices=('check', 'image', 'manifest'))
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--guest-memory-mib', type=int, required=True)
    parser.add_argument('--image', type=Path)
    parser.add_argument('--boot', type=Path)
    parser.add_argument('--out', type=Path)
    args = parser.parse_args()
    if not 1 <= args.guest_memory_mib < 16384:
        raise ValueError('DDR model requires guest memory in 1..16383 MiB')
    offset = args.guest_memory_mib * 1024**2
    files, size = inventory(args.source)
    if size > DDR_SIZE - offset:
        raise ValueError(f'Model exceeds DDR: model={size}, guest={offset}, DDR={DDR_SIZE}')
    print(f'[model-ddr] resources={sum(f["size"] for f in files)} image={size} offset={offset} PA={DDR_BASE+offset}')
    if args.mode == 'check':
        return
    if args.image is None:
        parser.error('image and manifest require --image')
    recorded = index(args.source, files, DDR_BASE + offset, size)
    if args.mode == 'image':
        args.image.parent.mkdir(parents=True, exist_ok=True)
        temporary = args.image.with_suffix('.new')
        with temporary.open('wb') as output:
            for item in files:
                output.seek(item['offset'])
                with (args.source / item['path']).open('rb') as source:
                    shutil.copyfileobj(source, output, 1024 * 1024)
            output.truncate(size)
        if inventory(args.source) != (files, size) or index(args.source, files, DDR_BASE + offset, size) != recorded:
            raise ValueError('Resources changed while packing DDR image')
        temporary.replace(args.image)
        recorded['image_sha256'] = digest(args.image)
        args.image.with_suffix('.files.json').write_text(json.dumps(recorded, indent=2) + '\n')
        rows = [f'BBMODEL1\t{DDR_BASE+offset}\t{size}\t{recorded["image_sha256"]}\n']
        rows.extend(f'{f["path"]}\t{f["offset"]}\t{f["size"]}\t{f["sha256"]}\n' for f in recorded['files'])
        args.image.with_suffix('.index').write_text(''.join(rows))
        return
    if args.boot is None or args.out is None:
        parser.error('manifest requires --boot and --out')
    recorded['image_sha256'] = digest(args.image)
    if json.loads(args.image.with_suffix('.files.json').read_text()) != recorded:
        raise ValueError('DDR image index differs from current resources')
    if args.image.stat().st_size != size or not 0 < args.boot.stat().st_size <= offset - FDT_SIZE:
        raise ValueError('Invalid boot/model image size or FDT overlap')
    loads = [{'role': role, 'file': str(file.resolve(strict=True)), 'offset': start,
              'size': file.stat().st_size, 'sha256': digest(file), 'format': 'binary'}
             for role, file, start in [('boot', args.boot, 0), ('model', args.image, offset)]]
    args.out.write_text(json.dumps({'version': 1, 'ddr_base': DDR_BASE, 'ddr_size': DDR_SIZE,
        'guest_memory_bytes': offset, 'model_base': DDR_BASE + offset, 'model_size': DDR_SIZE - offset,
        'fdt_base': DDR_BASE + offset - FDT_SIZE, 'fdt_size': FDT_SIZE, 'loads': loads}, indent=2) + '\n')

if __name__ == '__main__': main()
