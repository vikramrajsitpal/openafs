#!/usr/bin/python3

import lmdb
import sys
from collections import OrderedDict as od

fname = "ridb-dump.txt"

def interpret_key(key):
    vnode = int.from_bytes(key[0:4], "little")
    vunique = int.from_bytes(key[4:8], "little")
    name = str(key[8:].decode("utf-8")).rstrip('\x00')

    return (vnode, vunique, name)


def main():
    dbdir = sys.argv[1]
    env = lmdb.open(str(dbdir), readonly=True)

    db = od()

    with env.begin() as txn:
        with txn.cursor() as curs:
            for key, value in curs:
                v1,v2,n = interpret_key(key)
                db[(v1,v2,n)] = value.decode("utf-8").rstrip('\x00')
    
    #print("(VNODE, VUNIQUE, FILE_NAME): FILE_NAME\n")
    with open(fname, 'w+', encoding="utf-8") as f:
        for k,v in db.items():
            f.write(str(k) + ":" + v)



if __name__ == "__main__":
    if (len(sys.argv) != 2):
        print("Usage:\n" + sys.argv[0] + " <LMDB_DIR>")
        exit(1)
    main()
    
                
