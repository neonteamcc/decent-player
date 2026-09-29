#!/usr/bin/env python3
"""Append an executable init and static harness to an Alpine initramfs."""
import gzip,pathlib,stat,sys
base,binary,output=map(pathlib.Path,sys.argv[1:])
here=pathlib.Path(__file__).resolve().parent
archive=bytearray()
def add(name,content,mode):
    encoded=name.encode()+b'\0'
    fields=[1,mode,0,0,1,0,len(content),0,0,0,0,len(encoded),0]
    archive.extend(b'070701'+b''.join(f'{v:08x}'.encode() for v in fields)+encoded)
    archive.extend(b'\0'*(-len(archive)%4));archive.extend(content)
    archive.extend(b'\0'*(-len(archive)%4))
add('init',(here/'init').read_bytes(),stat.S_IFREG|0o755)
add('harness',binary.read_bytes(),stat.S_IFREG|0o755)
add('TRAILER!!!',b'',0)
output.write_bytes(base.read_bytes()+gzip.compress(archive,mtime=0))
