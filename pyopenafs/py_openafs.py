#!/usr/bin/python3

import socket
import sys
import types

from _py_openafs import ffi, lib

RXAFS_port = 7000
RXAFS_service_id = 1
RXAFS_service_name = "AFS"

RXAFSCB_port = 7001
RXAFSCB_service_id = 1
RXAFSCB_service_name = "afs"

RXAFSCB_ExecuteRequest = ffi.addressof(lib, 'RXAFSCB_ExecuteRequest')

UAEIO = 49733380
UAENOMEM = 49733387

CLIENT_CAPABILITY_ERRORTRANS = 0x1

class RxError(Exception):
    def __init__(self, code):
        self.code = code

    def __str__(self):
        return "Rx error %d" % self.code

rx_StartServer = lib.rx_StartServer

def rx_Init(host=0, port=0):
    code = lib.rx_InitHost(_host2rx(host), _port2rx(port))
    if code != 0:
        raise RxError(code)

def _host2rx(host):
    if isinstance(host, str):
        return int.from_bytes(socket.inet_aton(host), byteorder=sys.byteorder)
    else:
        return socket.htonl(host)

def _port2rx(port):
    return socket.htons(port)

def _str2c(data):
    return data.encode('utf-8')
    #return ffi.string(data).decode('utf-8')


def rx_NewConnection(host, port, service):
    # rxnull-only for now, so no secobj arguments

    host = _host2rx(host)
    port = _port2rx(port)

    sec_obj = lib.rxnull_NewClientSecurityObject()
    assert sec_obj is not None

    sec_idx = lib.RX_SECIDX_NULL

    conn = lib.rx_NewConnection(host, port, service, sec_obj, sec_idx)
    assert conn is not None

    return conn

def rx_DestroyConnection(conn):
    lib.rx_DestroyConnection(conn)

def _to_utf(s):
    if isinstance(s, str):
        return s.encode('utf-8')
    return s

def VL_GetEntryByNameU(conn, name):
    name = _to_utf(name)
    entry = ffi.new('struct uvldbentry *')

    code = lib.VL_GetEntryByNameU(conn, name, entry)
    if code != 0:
        raise RxError(code)

    return entry

def RXAFS_GetCapabilities(conn):
    caps = ffi.new('Capabilities *')

    code = lib.RXAFS_GetCapabilities(conn, caps)
    if code != 0:
        raise RxError(code)

    ret = []
    for cap_i in range(caps.Capabilities_len):
        ret.append(caps.Capabilities_val[cap_i])

    xdrs = ffi.new('XDR *')
    xdrs.x_op = lib.XDR_FREE
    lib.xdr_Capabilities(xdrs, caps)

    return ret

def RXAFS_FetchStatus(conn, a_fid):
    fid = ffi.new('AFSFid *')
    status = ffi.new('AFSFetchStatus *')
    callback = ffi.new('AFSCallBack *')
    sync = ffi.new('AFSVolSync *')

    fid.Volume = a_fid.volume
    fid.Vnode = a_fid.vnode
    fid.Unique = a_fid.unique

    code = lib.RXAFS_FetchStatus(conn, fid, status, callback, sync)
    if code != 0:
        raise RxError(code)

    # Don't bother returning any actual data yet; don't need it

def RXAFS_InverseLookup2(conn, a_fid):
    fid = ffi.new('AFSFid *')
    filename_p = ffi.new('char **')
    parent_fid_p = ffi.new('AFSFid *')

    fid.Volume = a_fid.volume
    fid.Vnode = a_fid.vnode
    fid.Unique = a_fid.unique

    code = lib.RXAFS_InverseLookup2(conn, fid, filename_p, parent_fid_p)
    if code != 0:
        raise RxError(code)

    filename = ffi.string(filename_p[0])
    parent_fid = types.SimpleNamespace(volume=parent_fid_p.Volume,
                                       vnode=parent_fid_p.Vnode,
                                       unique=parent_fid_p.Unique)

    return (filename, parent_fid)

_service_secobjs = []
def rx_NewServiceHost(host, port, serviceId, serviceName, serviceProc):
    host = _host2rx(host)
    port = _port2rx(port)

    sec_obj = lib.rxnull_NewServerSecurityObject()

    securityObjects = ffi.new('struct rx_securityClass *[1]')
    securityObjects[0] = sec_obj
    nSecurityObjects = 1

    service = lib.rx_NewServiceHost(host, port, serviceId, _str2c(serviceName), securityObjects, nSecurityObjects, serviceProc)
    if service == ffi.NULL:
        raise Exception("rx_NewServiceHost returned NULL")

    # Must keep a reference to this alive
    _service_secobjs.append(securityObjects)

    return service

SRXAFSCB_handler = None
lib.SRXAFSCB_TellMeAboutYourself_ptr = lib.SRXAFSCB_TellMeAboutYourself_handler
lib.SRXAFSCB_ProbeUuid_ptr           = lib.SRXAFSCB_ProbeUuid_handler
def rx_NewService_RXAFSCB(handler):
    global SRXAFSCB_handler
    SRXAFSCB_handler = handler
    print("Setting SRXAFSCB_handler to %r" % handler)

    service = rx_NewServiceHost(0, 0, RXAFSCB_service_id, RXAFSCB_service_name, RXAFSCB_ExecuteRequest)

def osi_alloc(n_items, n_bytes=1):
    ret = lib.osi_alloc(n_items * n_bytes)
    if ret == ffi.NULL:
        raise RxError(UAENOMEM)
    return ret

@ffi.def_extern(error=UAEIO)
def SRXAFSCB_TellMeAboutYourself_handler(call, addr, a_caps):
    handler = SRXAFSCB_handler
    print("Got TMAY, handler is %r" % handler)
    if handler is None:
        return lib.RXGEN_OPCODE

    try:
        uuid = handler.get_uuid(call)
        localips = handler.get_localips(call)
        caps = handler.get_caps(call)

        assert len(localips) > 0

        ffi.memmove(ffi.addressof(addr, 'uuid'),
                    uuid.bytes,
                    ffi.sizeof('afsUUID'))

        addr.numberOfInterfaces = len(localips)
        for ip_i in range(len(localips)):
            addr.addr_in[ip_i] = socket.ntohl(_host2rx(localips[ip_i]))

        a_caps.Capabilities_len = len(caps)
        if len(caps) > 0:
            a_caps.Capabilities_val = osi_alloc(ffi.sizeof('int32_t'), len(caps))
            for cap_i in range(len(caps)):
                a_caps.Capabilities_val[cap_i] = caps[cap_i]

    except RxError as exc:
        return exc.code
    return 0

@ffi.def_extern(error=UAEIO)
def SRXAFSCB_ProbeUuid_handler(call, a_uuid):
    handler = SRXAFSCB_handler
    print("Got ProbeUuid, handler is %r" % handler)
    if handler is None:
        return lib.RXGEN_OPCODE

    try:
        uuid = handler.get_uuid(call)
        a_uuid_bytes = bytes(ffi.buffer(a_uuid))
        if uuid.bytes != a_uuid_bytes:
            print("ProbeUuid mismatch: mine %s != arg %s" % (uuid.bytes, a_uuid_bytes))
            return 1
        print("ProbeUuid match: mine %s == arg %s" % (uuid.bytes, a_uuid_bytes))

    except RxError as exc:
        return exc.code
    return 0
