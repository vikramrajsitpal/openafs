#ifndef AFS_SRC_VICED_RIDB_H
#define AFS_SRC_VICED_RIDB_H

#include <afs/afsint.h>

struct okv_dbhandle;

/* ERROR CODES - FOR INTERNAL PURPOSES ONLY */

enum ridb_err_codes {
RIDB_BAD_KEY = 1,
RIDB_BAD_VAL,
RIDB_BAD_HDL,
RIDB_BAD_PATH,
RIDB_BAD_OPTS,
RIDB_ALREADY_OPEN
};

/* PROTOTYPES */

int ridb_create(const char *dir_path, struct okv_dbhandle** hdl);

int ridb_open(const char *dir_path, struct okv_dbhandle** hdl);

void ridb_close(struct db_handle** hdl);

int ridb_remove(char* dir_path);

int ridb_get(struct AFSFid* key, char** value);

int ridb_set(struct AFSFid* key, char* value);

int ridb_del(struct AFSFid* key, char** del_value);



#endif
