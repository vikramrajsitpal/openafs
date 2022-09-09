#!/usr/bin/python3

import cffi
import os.path
import socket
import sys

def main(argv):
    libdirs = argv[:]

    ffi = cffi.FFI()
    c_decls = \
"""
enum xdr_op {
    XDR_ENCODE = 0,
    XDR_DECODE = 1,
    XDR_FREE = 2
};
typedef struct XDR XDR;
struct XDR {
    enum xdr_op x_op;
    void *x_ops;
    void *x_public;
    void *x_private;
    void *x_base;
    uint32_t x_handy;
};

struct rx_securityClass;
struct rx_connection;
struct rx_call;

enum {
    RX_SECIDX_NULL = 0,
};

#define RXGEN_OPCODE -455
#define UAEIO 49733380

struct afsUUID {
    uint32_t time_low;
    uint16_t time_mid;
    uint16_t time_hi_and_version;
    char clock_seq_hi_and_reserved;
    char clock_seq_low;
    char node[6];
};
typedef struct afsUUID afsUUID;

struct AFSFid {
	uint32_t Volume;
	uint32_t Vnode;
	uint32_t Unique;
};
typedef struct AFSFid AFSFid;

struct AFSFetchStatus {
    uint32_t InterfaceVersion;
    uint32_t FileType;
    uint32_t LinkCount;
    uint32_t Length;
    uint32_t DataVersion;
    uint32_t Author;
    uint32_t Owner;
    uint32_t CallerAccess;
    uint32_t AnonymousAccess;
    uint32_t UnixModeBits;
    uint32_t ParentVnode;
    uint32_t ParentUnique;
    uint32_t ResidencyMask;
    uint32_t ClientModTime;
    uint32_t ServerModTime;
    uint32_t Group;
    uint32_t SyncCounter;
    uint32_t dataVersionHigh;
    uint32_t lockCount;
    uint32_t Length_hi;
    uint32_t errorCode;
};
typedef struct AFSFetchStatus AFSFetchStatus;

struct AFSCallBack {
    uint32_t CallBackVersion;
    uint32_t ExpirationTime;
    uint32_t CallBackType;
};
typedef struct AFSCallBack AFSCallBack;

struct AFSVolSync {
    uint32_t spare1;
    uint32_t spare2;
    uint32_t spare3;
    uint32_t spare4;
    uint32_t spare5;
    uint32_t spare6;
};
typedef struct AFSVolSync AFSVolSync;

#define AFS_MAX_INTERFACE_ADDR 32
struct interfaceAddr {
    int32_t numberOfInterfaces;
    afsUUID uuid;
    uint32_t addr_in[AFS_MAX_INTERFACE_ADDR];
    uint32_t subnetmask[AFS_MAX_INTERFACE_ADDR];
    uint32_t mtu[AFS_MAX_INTERFACE_ADDR];
};

typedef struct Capabilities {
    int32_t Capabilities_len;
    uint32_t *Capabilities_val;
} Capabilities;

int32_t xdr_Capabilities(XDR *xdrs, Capabilities *objp);

extern void *osi_alloc(int32_t x);
extern int32_t osi_free(void *x, int32_t size);

extern int32_t rx_InitHost(uint32_t host, uint32_t port);
extern void rx_StartServer(int donateMe);

extern struct rx_connection *rx_NewConnection(uint32_t host,
                                              uint16_t port,
                                              uint16_t service,
                                              struct rx_securityClass *security_object,
                                              int32_t security_index);
extern void rx_DestroyConnection(struct rx_connection *conn);

extern struct rx_securityClass *rxnull_NewServerSecurityObject(void);
extern struct rx_securityClass *rxnull_NewClientSecurityObject(void);

extern struct rx_service *rx_NewServiceHost(uint32_t host, uint16_t port,
                                            uint16_t serviceId,
                                            char *serviceName,
                                            struct rx_securityClass **securityObjects,
                                            int32_t nSecurityObjects,
                                            int32_t (*serviceProc)(struct rx_call *acall));

extern int32_t RXAFSCB_ExecuteRequest(struct rx_call *z_call);

extern int32_t RXAFS_GetCapabilities(struct rx_connection *z_conn, Capabilities *capabilities);
extern int32_t RXAFS_FetchStatus(struct rx_connection *z_conn, AFSFid *Fid,
                                 AFSFetchStatus *OutStat,
                                 AFSCallBack *CallBack, AFSVolSync *Sync);
extern int32_t RXAFS_InverseLookup2(struct rx_connection *z_conn, AFSFid *Fid,
                                    char **filename, AFSFid *ParentFid);

extern int32_t (*SRXAFSCB_TellMeAboutYourself_ptr)(struct rx_call *a_call,
                                                   struct interfaceAddr *addr,
                                                   Capabilities * capabilities);
extern int32_t (*SRXAFSCB_ProbeUuid_ptr)(struct rx_call *a_call,
                                         afsUUID *clientUuid);
"""
    cffi_decls = \
"""
extern "Python" int32_t SRXAFSCB_TellMeAboutYourself_handler(struct rx_call *a_call,
                                                             struct interfaceAddr *addr,
                                                             Capabilities * capabilities);
extern "Python" int32_t SRXAFSCB_ProbeUuid_handler(struct rx_call *a_call,
                                                   afsUUID *clientUuid);
"""

    c_src = \
"""
int32_t (*SRXAFSCB_TellMeAboutYourself_ptr)(struct rx_call *a_call,
                                            struct interfaceAddr *addr,
                                            Capabilities * capabilities);
int32_t (*SRXAFSCB_ProbeUuid_ptr)(struct rx_call *a_call,
                                  afsUUID *clientUuid);

int32_t
SRXAFSCB_TellMeAboutYourself(struct rx_call *a_call,
                             struct interfaceAddr *addr,
                             Capabilities * capabilities)
{
    if (SRXAFSCB_TellMeAboutYourself_ptr != NULL) {
        return (*SRXAFSCB_TellMeAboutYourself_ptr)(a_call, addr, capabilities);
    }
    return RXGEN_OPCODE;
}

int32_t
SRXAFSCB_ProbeUuid(struct rx_call *a_call,
                   afsUUID *clientUuid)
{
    if (SRXAFSCB_ProbeUuid_ptr != NULL) {
        return (*SRXAFSCB_ProbeUuid_ptr)(a_call, clientUuid);
    }
    return RXGEN_OPCODE;
}
"""
    opcode_rpcs = [
        'SRXAFSCB_GetLock',
        'SRXAFSCB_GetCE',
        'SRXAFSCB_XStatsVersion',
        'SRXAFSCB_GetXStats',
        'SRXAFSCB_WhoAreYou',
        'SRXAFSCB_GetServerPrefs',
        'SRXAFSCB_GetCellServDB',
        'SRXAFSCB_GetLocalCell',
        'SRXAFSCB_GetCacheConfig',
        'SRXAFSCB_GetCE64',
        'SRXAFSCB_GetCellByNum',
    ]
    ignore_rpcs = [
        'SRXAFSCB_CallBack',
        'SRXAFSCB_Probe',
        'SRXAFSCB_InitCallBackState',
        'SRXAFSCB_InitCallBackState2',
        'SRXAFSCB_InitCallBackState3',
    ]

    for rpc in opcode_rpcs:
        c_src += "int32_t\n" + rpc + "(void)\n{\nreturn RXGEN_OPCODE;\n}\n";
    for rpc in ignore_rpcs:
        c_src += "int32_t\n" + rpc + "(void)\n{\nreturn 0;\n}\n";

    ffi.cdef(c_decls + cffi_decls)

    ffi.set_source("_py_openafs", c_decls + c_src,
                   libraries=['afsint', 'afsauthent_pic', 'afsrpc_pic', 'rokenafs', 'afshcrypto'],
                   library_dirs=libdirs)

    ffi.compile(verbose=True)

if __name__ == '__main__':
    main(sys.argv[1:])
