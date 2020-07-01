#include <stdio.h>
#include <lmdb.h>

int
main(void)
{
    printf("Address of mdb_env_open is %p\n", (void*)mdb_env_open);
    return 0;
}
