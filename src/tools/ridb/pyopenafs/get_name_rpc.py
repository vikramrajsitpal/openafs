#!/usr/bin/python3

import sys
import types

import py_openafs as rx

def main(argv):
    rx.rx_Init(0, 0)
    conn = rx.rx_NewConnection('127.0.0.1', rx.RXAFS_port, rx.RXAFS_service_id)
    
    f = argv[0].strip().split(".")
    if (len(f) != 3):
        print("Error in parsing arg: ", argv[0])
        print("Format: \"volume.vnode.unique\"")
        exit(-1)
    fid = types.SimpleNamespace(volume=int(f[0]), vnode=int(f[1]), unique=int(f[2]))
    #print("Sent: %d.%d.%d" %(fid.volume, fid.vnode, fid.unique))
    try:
        (fname, parent) = rx.RXAFS_InverseLookup2(conn, fid)
        pfid = "%d.%d.%d" %(parent.volume, parent.vnode, parent.unique)
        print("Received: Filename: %s Parent: %s" % (fname.decode('utf-8'),pfid ))
    except rx.RxError as err:
        print("Received:", err)

if __name__ == '__main__':
    main(sys.argv[1:])
