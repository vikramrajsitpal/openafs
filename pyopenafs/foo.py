#!/usr/bin/python3

import sys
import types

import py_openafs as rx

def main(argv):
    rx.rx_Init(0, 0)
    conn = rx.rx_NewConnection('127.0.0.1', rx.RXAFS_port, rx.RXAFS_service_id)
    
    f = argv[0].strip().split(":")
    fid = types.SimpleNamespace(volume=int(f[0]), vnode=int(f[1]), unique=int(f[2]))
    print(fid)
    (fname, parent) = rx.RXAFS_InverseLookup2(conn, fid)
    print("Received: fname %r, parent %r\n" % (fname, parent))

if __name__ == '__main__':
    main(sys.argv[1:])
