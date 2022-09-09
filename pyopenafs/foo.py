#!/usr/bin/python3

import sys
import types

import py_openafs as rx

def main(argv):
    rx.rx_Init(0, 0)

    conn = rx.rx_NewConnection('127.0.0.1', rx.RXAFS_port, rx.RXAFS_service_id)

    fid = types.SimpleNamespace(volume=2, vnode=1, unique=1)

    (fname, parent) = rx.RXAFS_InverseLookup2(conn, fid)

    print("got fname %r, parent %r" % (fname, parent))

if __name__ == '__main__':
    main(sys.argv[1:])
