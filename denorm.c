/*
 *
 * This file implements Alchemy's DENORM command
 *

GPL License

Copyright (c) 2010 Russell Sullivan <jaksprats AT gmail DOT com>
ALL RIGHTS RESERVED 

   This file is part of AlchemyDatabase

    AlchemyDatabase is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    AlchemyDatabase is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with AlchemyDatabase.  If not, see <http://www.gnu.org/licenses/>.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "redis.h"

#include "bt_iterator.h"
#include "rpipe.h"
#include "parser.h"
#include "aobj.h"
#include "alsosql.h"

// FROM redis.c
#define RL4 redisLog(4,
extern struct sharedObjectsStruct shared;
extern struct redisServer server;

extern r_tbl_t  Tbl[MAX_NUM_DB][MAX_NUM_TABLES];

void denormCommand(redisClient *c) {
    TABLE_CHECK_OR_REPLY(c->argv[1]->ptr,)
    sds wildcard = c->argv[2]->ptr;
    if (!strchr(wildcard, '*')) {
        addReply(c, shared.denorm_wildcard_no_star);
        return;
    }

    uint32 wlen = sdslen(wildcard);
    uint32 spot = 0;
    for (uint32 i = 0; i < wlen; i++) {
        if (wildcard[i] == '*') {
            spot = i;
            break;
        }
    }
    uint32  restlen  = (spot < wlen - 2) ? wlen - spot - 1: 0;
    sds     s_wldcrd = sdsnewlen(wildcard, spot);
    s_wldcrd         = sdscatlen(s_wldcrd, "%s", 2);
    if (restlen) s_wldcrd = sdscatlen(s_wldcrd, &wildcard[spot + 1], restlen);
    sds     d_wldcrd = sdsdup(s_wldcrd);
    char   *fmt      = strstr(d_wldcrd, "%s"); /* written 2 lines up cant fail*/
    fmt++;
    *fmt             = 'd'; /* changes "%s" into "%d" - FIX: too complicated */

    btEntry     *be;
    robj        *argv[4]; // TODO this can be done natively, no need for Client
    redisClient *fc   = rsql_createFakeClient();
    fc->argv          = argv;
    fc->argc          = 4;
    ulong        card = 0;
    robj        *btt  = lookupKeyRead(c->db, Tbl[server.dbid][tmatch].name);
    bt          *btr  = (bt *)btt->ptr;
    btSIter     *bi   = btGetFullRangeIterator(btr);
    while ((be = btRangeNext(bi)) != NULL) {      // iterate btree
        aobj *apk  = be->key;
        void *rrow = be->val;

        sds hname = sdsempty();
        if (apk->type == COL_TYPE_STRING) {
            hname = sdscatprintf(hname, s_wldcrd, apk->s);
        } else {
            hname = sdscatprintf(hname, d_wldcrd, apk->i);
        }
        fc->argv[1] = createStringObject(hname, sdslen(hname));
        sdsfree(hname);

        /* PK is in name */
        for (int i = 1; i < Tbl[server.dbid][tmatch].col_count; i++) {
            sds  tname  = Tbl[server.dbid][tmatch].col_name[i]->ptr;
            fc->argv[2] = createStringObject(tname, sdslen(tname));
            aobj rcol   = getRawCol(btr, rrow, i, apk, tmatch, NULL, 1);
            fc->argv[3] = _createStringObject(rcol.s);
            releaseAobj(&rcol);
            rsql_resetFakeClient(fc);
            hsetCommand(fc); /* does this free argv[*}? possible MEM_LEAK */
        }
        card++;
    }

    addReplyLongLong(c, card);
    sdsfree(s_wldcrd);
    sdsfree(d_wldcrd);
    rsql_freeFakeClient(fc);
}
