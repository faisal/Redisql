/*
 * This file implements the sql parsing routines for Alsosql
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
char *strcasestr(const char *haystack, const char *needle); /*compiler warning*/

#include "sds.h"
#include "zmalloc.h"
#include "redis.h"

#include "join.h"
#include "index.h"
#include "cr8tblas.h"
#include "colparse.h"
#include "rpipe.h"
#include "parser.h"
#include "alsosql.h"
#include "common.h"
#include "wc.h"

// FROM redis.c
extern struct sharedObjectsStruct shared;
extern struct redisServer server;

extern int      Num_tbls[MAX_NUM_DB];
extern r_tbl_t  Tbl     [MAX_NUM_DB][MAX_NUM_TABLES];
extern r_ind_t  Index   [MAX_NUM_DB][MAX_NUM_INDICES];
extern stor_cmd AccessCommands[];
extern uchar    OP_len[7];

char   *Ignore_keywords[]   = {"PRIMARY", "CONSTRAINT", "UNIQUE", "KEY",
                               "FOREIGN" };
uint32  Num_ignore_keywords = 5;

static char CLT = '<';
static char CGT = '>';
static char CEQ = '=';
static char CNE = '!';

/* CREATE_TABLE_HELPERS CREATE_TABLE_HELPERS CREATE_TABLE_HELPERS */
bool ignore_cname(char *tkn) {
    for (uint32 i = 0; i < Num_ignore_keywords; i++) {
        if (!strncasecmp(tkn, Ignore_keywords[i], strlen(Ignore_keywords[i]))) {
            return 1;
        }
    }
    return 0;
}

/* SYNTAX: CREATE TABLE t (id int , division INT,salary FLOAT, name TEXT)*/
bool parseCreateTable(redisClient *c,
                      char          cnames[][MAX_COLUMN_NAME_SIZE],
                      int          *ccount,
                      sds           col_decl) {
    char *token = col_decl;
    if (*token == '(') token++;
    if (!*token) { /* empty or only '(' */
        addReply(c, shared.createsyntax);
        return 0;      
    }
    SKIP_SPACES(token)
    while (token) {
        if (*ccount == MAX_COLUMN_PER_TABLE) {
            addReply(c, shared.toomanycolumns);
            return 0;
        }
        int clen;
        while (token) { /* first parse column name */
            clen      = get_token_len(token);
            token     = rem_backticks(token, &clen);
            if (!ignore_cname(token)) break;
            token = get_next_token_nonparaned_comma(token);
        }
        if (!token) break;
        if (!cCpyOrReply(c, token, cnames[*ccount], clen)) return 0;

        token       = next_token_delim3(token, ',', ')'); /* parse ctype*/
        if (!token) break;
        sds   type  = sdsnewlen(token, get_tlen_delim3(token, ',', ')'));
        token       = get_next_token_nonparaned_comma(token);

        /* in type search for INT (but not BIGINT - too big @8 Bytes) */
        int ntbls = Num_tbls[server.dbid];
        if ((strcasestr(type, "INT") && !strcasestr(type, "BIGINT")) ||
                   strcasestr(type, "TIMESTAMP")) {
            Tbl[server.dbid][ntbls].col_type[*ccount] = COL_TYPE_INT;
        } else if (strcasestr(type, "FLOAT") ||
                   strcasestr(type, "REAL")  ||
                   strcasestr(type, "DOUBLE")) {
            Tbl[server.dbid][ntbls].col_type[*ccount] = COL_TYPE_FLOAT;
        } else if (strcasestr(type, "CHAR") ||
                   strcasestr(type, "TEXT")  ||
                   strcasestr(type, "BLOB")  ||
                   strcasestr(type, "BYTE")  ||
                   strcasestr(type, "BINARY")) {
            Tbl[server.dbid][ntbls].col_type[*ccount] = COL_TYPE_STRING;
        } else {
            addReply(c, shared.undefinedcolumntype);
            return 0;
        }
        sdsfree(type);
        Tbl[server.dbid][ntbls].col_flags[*ccount] = 0; /* TODO flags */
        *ccount                                    = *ccount + 1;
    }
    return 1;
}

/* WHERE_CLAUSE WHERE_CLAUSE WHERE_CLAUSE WHERE_CLAUSE WHERE_CLAUSE */
/* WHERE_CLAUSE WHERE_CLAUSE WHERE_CLAUSE WHERE_CLAUSE WHERE_CLAUSE */

/* SYNTAX: BETWEEN x AND y */
static uchar parseRangeReply(redisClient  *c,
                             char         *first,
                             cswc_t       *w,
                             char        **finish) {
    int   slen;
    char *and    = strchr(first, ' ');
    if (!and) goto parse_range_err;
    int   flen   = and - first;
    SKIP_SPACES(and) /* find start of value */
    if (strncasecmp(and, "AND ", 4)) {
        addReply(c, shared.whereclause_no_and);
        return SQL_ERR_LOOKUP;
    }
    char *second = next_token(and);
    if (!second) goto parse_range_err;
    char *tokfin = strchr(second, ' ');
    if (tokfin) {
        slen    = tokfin - second;
        SKIP_SPACES(tokfin)
        *finish = tokfin;
    } else {
        slen    = strlen(second);
        *finish = NULL;
    }
    w->low  = createStringObject(first,  flen);
    w->high = createStringObject(second, slen);
    return SQL_RANGE_QUERY;

parse_range_err:
    addReply(c, shared.whereclause_between);
    return SQL_ERR_LOOKUP;
}

/* "OFFSET M" if M is a redis variable, this is a cursor call */
static bool setOffsetReply(redisClient *c, cswc_t *w, char *nextp) {
    if (isalpha(*nextp)) { /* OFFSET "var" - used in cursors */
        int   len  = get_token_len(nextp);
        robj *ovar = createStringObject(nextp, len);
        w->ovar    = sdsdup(ovar->ptr);
        robj *o    = lookupKeyRead(c->db, ovar);
        decrRefCount(ovar);
        if (o) {
            long long value;
            if (!checkType(c, o, REDIS_STRING) &&
                 getLongLongFromObjectOrReply(c, o, &value,
                            "OFFSET variable is not an integer") == REDIS_OK) {
                w->ofst = (long)value;
            } else { /* possibly variable was a ZSET,LIST,etc */
                sdsfree(w->ovar);
                w->ovar = NULL;
                return 0;
            }
        }
    } else {
        w->ofst = atol(nextp); /* LIMIT N OFFSET X */
    }
    return 1;
}

#define PARSEOB_DONE(endt)   \
    if (!endt || !(*endt)) { \
        *more   = 0;         \
        *finish = NULL;      \
        *token  = NULL;      \
        return 1;            \
    }

#define PARSEOB_IFCOMMA_NEXTTOK(endt) \
    if (*endt == ',') {               \
        endt++;                       \
        PARSEOB_DONE(endt)            \
        SKIP_SPACES(endt)             \
        PARSEOB_DONE(endt)            \
        *more  = 1;                   \
        *token = endt;                \
        return 1;                     \
    }

/* SYNTAX: ORDER BY {col [DESC],}+ [LIMIT n [OFFSET m]] */
static bool parseOBYcol(redisClient  *c,
                        char        **token,
                        int           tmatch,
                        cswc_t       *w,
                        char        **finish,
                        bool         *more) {
    int   tlen  = get_tlen_delim2(*token, ',');
    if (!tlen) tlen = (int)strlen(*token);
    char *prd   = _strnchr(*token, '.', tlen);
    if (prd) { /* JOIN - NOTE: currently only support single column ORDER BY */
        if ((w->obt[w->nob] = find_table_n(*token, prd - *token)) == -1) {
            addReply(c, shared.join_order_by_tbl);
            return 0;
        }
        char *cname = prd + 1;
        int   clen  = get_token_len(cname); /* no COMMA delimiting for JOINS */
        if (!clen) {
            addReply(c, shared.join_order_by_col);
            return 0;
        }
        w->obc[w->nob] = find_column_n(w->obt[w->nob], cname, clen);
        if (w->obc[w->nob] == -1) {
            addReply(c, shared.join_order_by_col);
            return 0;
        }
    } else {  /* RANGE QUERY */
        w->obc[w->nob] = find_column_n(tmatch, *token, tlen);
        if (w->obc[w->nob] == -1) {
            addReply(c, shared.order_by_col_not_found);
            return 0;
        }
    }
    w->asc[w->nob] = 1; /* ASC by default */
    *more          = (*(*token + tlen) == ',') ? 1: 0;
    int nob        = w->nob;
    w->nob++;           /* increment as parsing may already be finished */

    char *nextt = next_token_delim2(*token, ',');
    PARSEOB_DONE(nextt)            /* e.g. "ORDER BY X" no DESC nor COMMA    */
    PARSEOB_IFCOMMA_NEXTTOK(nextt) /* e.g. "ORDER BY col1,col2,col3"         */
    SKIP_SPACES(nextt)             /* e.g. "ORDER BY col1    DESC" -> "DESC" */
    PARSEOB_IFCOMMA_NEXTTOK(nextt) /* e.g. "ORDER BY col1    ,   col2"       */
    PARSEOB_DONE(nextt)            /* e.g. "ORDER BY X   " no DESC nor COMMA */

    if (!strncasecmp(nextt, "DESC", 4)) {
        w->asc[nob] = 0;
        if (*(nextt + 4) == ',') *more = 1;
        nextt       = next_token_delim2(nextt, ',');
        PARSEOB_DONE(nextt) /* for misformed ORDER BY ending in ',' */
    } else if (!strncasecmp(nextt, "ASC", 3)) {
        w->asc[nob] = 1;
        if (*(nextt + 3) == ',') *more = 1;
        nextt       = next_token_delim2(nextt, ',');
        PARSEOB_DONE(nextt) /* for misformed ORDER BY ending in ',' */
    }
    PARSEOB_IFCOMMA_NEXTTOK(nextt) /* e.g. "ORDER BY c1 DESC ,c2" -> "c2" */
    *token = nextt;
    return 1;
}

static bool parseOrderBy(redisClient  *c,
                         char         *by,
                         int           tmatch,
                         cswc_t       *w,
                         char        **finish) {
    if (strncasecmp(by, "BY ", 3)) {
        addReply(c, shared.whereclause_orderby_no_by);
        return 0;
    }
    char *token = next_token(by);
    if (!token) {
        addReply(c, shared.whereclause_orderby_no_by);
        return 0;
    }
    bool more = 1; /* more OBC to parse */
    while (more) {
        if (w->nob == MAX_ORDER_BY_COLS) {
            addReply(c, shared.toomany_nob);
            return 0;
        }
        if (!parseOBYcol(c, &token, tmatch, w, finish, &more)) return 0;
    }

    if (token) {
        if (!strncasecmp(token, "LIMIT ", 6)) {
            token  = next_token(token);
            if (!token) {
                addReply(c, shared.orderby_limit_needs_number);
                return 0;
            }
            w->lim = atol(token);
            token  = next_token(token);
            if (token) {
                if (!strncasecmp(token, "OFFSET", 6)) {
                    token  = next_token(token);
                    if (!token) {
                        addReply(c, shared.orderby_offset_needs_number);
                        return 0;
                    }
                    if (!setOffsetReply(c, w, token)) return 0;
                    token   = next_token(token);
                }
            }
        }
    }
    if (token) *finish = token; /* still something to parse - maybe "STORE" */
    return 1;
}

bool parseWCAddtlSQL(redisClient *c, char *token, cswc_t *w) {
    bool check_sto = 1;
    w->lvr         = token;   /* assume parse error */
    if (!strncasecmp(token, "ORDER ", 6)) {
        char *by      = next_token(token);
        if (!by) {
            w->lvr = NULL;
            addReply(c, shared.whereclause_orderby_no_by);
            return 0;
        }
        char *lfin    = NULL;
        if (!parseOrderBy(c, by, w->tmatch, w, &lfin)) {
            w->lvr = NULL;
            return 0;
        }
        if (lfin) {
            check_sto = 1;
            token     = lfin;
        } else {
            check_sto = 0;
            w->lvr    = NULL; /* negate parse error */
        }
    }
    if (check_sto) {
        w->lvr = token; /* assume parse error */
        if (!strncasecmp(token, "STORE ", 6)) {
            w->wtype += SQL_STORE_LOOKUP_MASK;
            w->lvr    = NULL;   /* negate parse error */
        }
    }
    return 1;
}

static bool addRCmdToINList(redisClient *c,
                            void        *x,
                            robj        *key,
                            long        *card,
                            int          b,   /* variable ignored */
                            int          n) { /* variable ignored */
    c = NULL; b = 0; n = 0; /* compiler warnings */
    list **inl = (list **)x;
    listAddNodeTail(*inl, key);
    *card      = *card + 1;
    return 1;
}

#define IN_RCMD_ERR_MSG \
  "-ERR SELECT FROM WHERE col IN(Redis_cmd) - inner command had error: "

static bool parseWC_IN_NRI(redisClient *c, list **inl, char *s, int slen) {
    int   axs  = getAccessCommNum(s);
    if (axs == -1 ) {
        addReply(c, shared.accesstypeunknown);
        return 0;
    }
    int     argc;
    robj **rargv;
    if (AccessCommands[axs].parse) { /* SELECT has 6 args */
        sds x = sdsnewlen(s, slen);
        rargv = (*AccessCommands[axs].parse)(x, &argc);
        sdsfree(x);
        if (!rargv) {
            addReply(c, shared.where_in_select);
            return 0;
        }
    } else {
        sds *argv  = sdssplitlen(s, slen, " ", 1, &argc);
        int  arity = AccessCommands[axs].argc;
        if ((arity > 0 && arity != argc) || (argc < -arity)) { 
            zfree(argv);
            addReply(c, shared.accessnumargsmismatch);
            return 0;
        }
        rargv = zmalloc(sizeof(robj *) * argc);
        for (int j = 0; j < argc; j++) {
            rargv[j] = createStringObject(argv[j], sdslen(argv[j]));
        }
        zfree(argv);
    }
    redisClient *rfc = rsql_createFakeClient();
    rfc->argv        = rargv;
    rfc->argc        = argc;

    uchar f   = 0;
    fakeClientPipe(c, rfc, inl, 0, &f, addRCmdToINList, emptyNoop);
    bool  err = 0;
    if (!replyIfNestedErr(c, rfc, IN_RCMD_ERR_MSG)) err = 1;

    rsql_freeFakeClient(rfc);
    zfree(rargv);
    return !err;
}

static char *checkIN_Clause(redisClient *c, char *token) {
    char *end = str_matching_end_paren(token);
    if (!end || (*token != '(')) {
        addReply(c, shared.whereclause_in_err);
        return NULL;
    }
    return end;
}
/* SYNTAX: IN (a,b,c) OR IN($redis_command arg1 arg2) */
static uchar parseWC_IN(redisClient  *c,
                        char         *token,
                        list        **inl,
                        uchar         ctype,
                        char        **finish) {

    char *end = checkIN_Clause(c, token);
    if (!end) return SQL_ERR_LOOKUP;
    *inl       = listCreate();
    bool piped = 0;
    token++;
    if (*token == '$') piped = 1;

    if (piped) {
        char *s    = token + 1;
        int   slen = end - s;
        if (!parseWC_IN_NRI(c, inl, s, slen)) return SQL_ERR_LOOKUP;
    } else {
        char *s   = token;
        char *beg = s;
        while (1) { /* the next line can search beyond end, safe but lame */
            char *nextc = str_next_unescaped_chr(beg, s, ',');
            if (!nextc || nextc > end) break;
            SKIP_SPACES(s)
            robj *r     = createStringObject(s, nextc - s);
            listAddNodeTail(*inl, r);
            nextc++;
            s           = nextc;
            SKIP_SPACES(s)
        }
        robj *r = createStringObject(s, end - s);
        listAddNodeTail(*inl, r);
    }

    convertINLtoAobj(inl, ctype); /* convert from STRING to ctype */

    end++;
    if (*end) {
        SKIP_SPACES(end)
        *finish = end;
    }
    return SQL_IN_LOOKUP;
}

/* Parse "tablename.columnname" into [tmatch, cmatch, imatch */
static bool parseInumTblCol(redisClient *c,
                            int         *tmatch,
                            char        *token,
                            int          tlen,
                            int         *cmatch,
                            int         *imatch,
                            bool         is_scan) {
    is_scan = 0; /* compiler warning */
    char *nextp = _strnchr(token, '.', tlen);
    if (!nextp) {
        addReply(c, shared.badindexedcolumnsyntax);
        return 0;
    }
    *tmatch = find_table_n(token, nextp - token);
    if (*tmatch == -1) {
        addReply(c, shared.nonexistenttable);
        return 0;
    }
    nextp++;
    *cmatch = find_column_n(*tmatch, nextp, tlen - (nextp - token));
    if (*cmatch == -1) {
        addReply(c,shared.nonexistentcolumn);
        return 0;
    }
    *imatch = find_index(*tmatch, *cmatch);
    if (*imatch == -1) {
        addReply(c, shared.whereclause_col_not_indxd);
        return 0;
    }
    return 1;
}

/* Parse "columnname" from table Tbl[tmatch] into [cmatch, imatch */
static bool parseInumCol(redisClient *c,
                        int         *tmatch,
                        char        *token,
                        int          tlen,
                        int         *cmatch,
                        int         *imatch,
                        bool         needi) {
    *cmatch = find_column_n(*tmatch, token, tlen);
    if (*cmatch == -1) {
        addReply(c, shared.whereclause_col_not_found);
        return 0;
    }
    *imatch = (*cmatch == -1) ? -1 : find_index(*tmatch, *cmatch); 
    if (needi && *imatch == -1) { /* non-indexed column */
        addReply(c, shared.whereclause_col_not_indxd);
        return 0;
    }
    return 1;
}

static enum OP findOperator(char *val, uint32 vlen, char **spot) {
    for (uint32 i = 0; i < vlen; i++) {
        char x = val[i];
        if (x == CEQ) {
            *spot  = val + i;
            return EQ;
        }
        if (x == CLT) {
            if (i != (vlen - 1) && val[i + 1] == CEQ) {
                *spot  = val + i + 1;
                return LE;
            } else {
                *spot  = val + i;
                return LT;
            }
        }
        if (x == CGT) {
            if (i != (vlen - 1) && val[i + 1] == CEQ) {
                *spot  = val + i + 1;
                return GE;
            } else {
                *spot  = val + i;
                return GT;
            }
        }
        if (x == CNE) {
            if (i != (vlen - 1) && val[i + 1] == CEQ) {
                *spot  = val + i + 1;
                return NE;
            }
        }
    }
    *spot = NULL;
    return NONE;
}

/* SYNTAX Where Relational Token
     1.) "col = 4"
     2.) "col BETWEEN x AND y"
     3.) "col IN (,,,,,)" */
static uchar parseWCTokenRelation(redisClient  *c,
                                  cswc_t       *w,
                                  uchar         sop,
                                  char         *token,
                                  char        **finish,
                                  bool          is_scan,
                                  f_t          *flt,
                                  bool          ifound,
                                  bool          is_join) {
    int              idum;
    int              two_tok_len;
    uchar            wtype = SQL_ERR_LOOKUP;
    parse_inum_func *pif   = (is_join) ? parseInumTblCol : parseInumCol;
    char            *tok2  = next_token(token);
    if (!tok2) two_tok_len = strlen(token);
    else       two_tok_len = (tok2 - token) + get_token_len(tok2);

    char    *spot = NULL;
    enum OP  op   = findOperator(token, two_tok_len, &spot);
    if (op != NONE) { /* "col=X", "col !=X", "col <= X" */
        char *end = spot - OP_len[op];;
        while (isblank(*end)) end--; /* find end of PK */
        if (is_scan || ifound) { /* set filter.[cmatch,op] */
            if (!(*pif)(c, &w->tmatch, token, end - token + 1, &flt->cmatch,
                        &idum, 0)) return SQL_ERR_LOOKUP;
            if (flt->cmatch == 0) { /* NO filters on PK */
                addReply(c, shared.pk_filter);
                return SQL_ERR_LOOKUP;
            }
            flt->op = op;
        } else {      /* set w->[t/c/imatch] */
            if (op != EQ) {
                addReply(c, shared.pk_query_mustbe_eq);
                return SQL_ERR_LOOKUP;
            }
            if (!(*pif)(c, &w->tmatch, token, end - token + 1, &w->cmatch,
                        &w->imatch, !is_scan)) return SQL_ERR_LOOKUP;
        }
        wtype        = w->cmatch ? SQL_SINGLE_FK_LOOKUP : SQL_SINGLE_LOOKUP;
        char *start  = spot + 1;
        SKIP_SPACES(start) /* find start of value */
        if (!*start) goto sql_tok_rel_err;
        char *tokfin = strchr(start, ' ');
        int   len    = tokfin ? tokfin - start : (uint32)strlen(start);
        if (is_scan || ifound) { /* set filter.rhs */
            flt->rhs = sdsnewlen(start, len);
        } else {                 /* set w.key */
            w->key   = createStringObject(start, len); //TODO -> aobj
        }
        if (tokfin) SKIP_SPACES(tokfin)
        *finish = tokfin;
        if (!tokfin) { /* nutn 2 parse */
            w->lvr = NULL;
            return wtype;
        }
    } else { /* RANGE_QUERY or IN_QUERY */
        char *nextp = strchr(token, ' ');
        if (!nextp) goto sql_tok_rel_err;
        int imatch = -1;
        int cmatch = -1;
        if (!(*pif)(c, &w->tmatch, token, nextp - token,
                    &cmatch, &imatch, !is_scan)) return SQL_ERR_LOOKUP;
        SKIP_SPACES(nextp) /* find start of next token */
        if (!strncasecmp(nextp, "IN ", 3)) {
            nextp = next_token(nextp);
            if (!nextp) goto sql_tok_rel_err;
            if (!ifound && !is_scan) w->imatch = imatch;
            uchar ctype = Tbl[server.dbid][w->tmatch].col_type[cmatch];
            list *inl   = NULL;
            wtype       = parseWC_IN(c, nextp, &inl, ctype, finish);
            if (wtype == SQL_ERR_LOOKUP) return SQL_ERR_LOOKUP;
            if (ifound || is_scan) {
                flt->op     = EQ;
                flt->inl    = inl;
                flt->cmatch = cmatch;
            } else {
                w->inl      = inl;
            }
        } else if (!strncasecmp(nextp, "BETWEEN ", 8)) { /* RANGE QUERY */
            nextp = next_token(nextp);
            if (!nextp) goto sql_tok_rel_err;
            if (!ifound && !is_scan) w->imatch = imatch;
            wtype     = parseRangeReply(c, nextp, w, finish);
            w->cmatch = cmatch; /* scan uses this */
            if (wtype == SQL_ERR_LOOKUP) return SQL_ERR_LOOKUP;
        } else {
            goto sql_tok_rel_err;
        }
    }
    return wtype;

sql_tok_rel_err:
    if      (sop == SQL_SELECT) addReply(c, shared.selectsyntax);
    else if (sop == SQL_DELETE) addReply(c, shared.deletesyntax);
    else if (sop == SQL_UPDATE) addReply(c, shared.updatesyntax);
    else       /* SCANSELECT */ addReply(c, shared.scanselectsyntax);
    return SQL_ERR_LOOKUP;
}

/* NOTE "BETWEEN"s produce 2 filters, so they must be parsed in parseWCReply */
static void addLowHighToFlist(cswc_t *w) {
    f_t *f_low  = newFilter(w->cmatch, GE, w->low->ptr);
    listAddNodeTail(w->flist, f_low);
    f_t *f_high = newFilter(w->cmatch, LE, w->high->ptr);
    listAddNodeTail(w->flist, f_high);
    decrRefCount(w->low);
    w->low = NULL;
    decrRefCount(w->high);
    w->high = NULL;
}

/* FILTER_PARSING FILTER_PARSING FILTER_PARSING FILTER_PARSING */
void parseWCReply(redisClient *c, cswc_t *w, uchar sop, bool is_scan) {
    w->wtype     = SQL_ERR_LOOKUP;
    char *finish = w->token;
    sds   token  = NULL;
    bool  ifound = 0;
    while (1) {
        f_t   flt;
        initFilter(&flt);
        //TODO needs to be str_case_unescaped_quotes_str(" AND ")
        char *tokfin = NULL;
        char *in     = strcasestr(finish, " IN ");
        char *and    = strcasestr(finish, " AND ");
        if (and && in && in < and) { /* include "IN (..........) */
            and = in + 4;
            SKIP_SPACES(and)
            if (!*and) goto p_wd_end;
            and = checkIN_Clause(c, and); /* and now @ ')' */
            if (!and) goto p_wd_end;
            and = strcasestr(and, " AND ");
        } else {                     /* include "BETWEEN x AND y" */
            char *btwn = strcasestr(finish, " BETWEEN ");
            if (and && btwn && btwn < and) and = strcasestr(and + 5, " AND ");
        }
        int   tlen   = and ? and - finish : (int)strlen(finish);
        if (token) sdsfree(token);
        token         = sdsnewlen(finish, tlen);
        uchar t_wtype = parseWCTokenRelation(c, w, sop, token, &tokfin,
                                             is_scan, &flt, ifound, 0);
        if (t_wtype == SQL_ERR_LOOKUP) goto p_wd_end;

        if (is_scan) {
            w->wtype = t_wtype;
            if (!w->flist) w->flist = listCreate();
            if (w->wtype == SQL_RANGE_QUERY) addLowHighToFlist(w);
            else                     listAddNodeTail(w->flist, cloneFilt(&flt));
            releaseFilter(&flt);
        } else {
            if (ifound) { /* index first token, then filters  */
                if (!w->flist) w->flist = listCreate();
                if (t_wtype == SQL_RANGE_QUERY) addLowHighToFlist(w);
                else                 listAddNodeTail(w->flist, cloneFilt(&flt));
                releaseFilter(&flt);
            } else {
                w->wtype = t_wtype;
                ifound   = 1;
            }
        }

        if (and) {
            finish = and + 5; /* go past " AND " */
            SKIP_SPACES(finish)
            if (!*finish) break;
        } else {
            if (!tokfin) break;
            finish = tokfin;
            SKIP_SPACES(finish)
            if (!*finish) break;
            if (!parseWCAddtlSQL(c, finish, w)) goto p_wd_end;
            break;
        }
    }
    convertFilterListToAobj(w->flist, w->tmatch);

p_wd_end:
    if (w->lvr) { /* blank space in lvr is not actually lvr */
        SKIP_SPACES(w->lvr)
        if (*(w->lvr)) w->lvr = sdsnewlen(w->lvr, strlen(w->lvr));
        else           w->lvr = NULL;
    }
    if (token) sdsfree(token);
}

/* JOIN JOIN JOIN JOIN JOIN JOIN JOIN JOIN JOIN JOIN JOIN JOIN JOIN JOIN */
/* JOIN JOIN JOIN JOIN JOIN JOIN JOIN JOIN JOIN JOIN JOIN JOIN JOIN JOIN */
static bool addInd2Join(redisClient *c,
                        int          ind,
                        int         *n_ind,
                        int          j_indxs[MAX_JOIN_INDXS]) {
    if (*n_ind == MAX_JOIN_INDXS) {
        addReply(c, shared.toomanyindicesinjoin);
        return 0;
    }
    for (int i = 0; i < *n_ind; i++) {
        if (j_indxs[i] == ind) return 1;
    }
    j_indxs[*n_ind] = ind;
    *n_ind          = *n_ind + 1;
    return 1;
}

/* TODO clean this up when the join engine gets cleaned up */
static void singleFKHack(cswc_t *w, uchar *wtype) {
    *wtype = SQL_RANGE_QUERY;
    w->low = cloneRobj(w->key);
    w->high = cloneRobj(w->key);
}
/* SYNTAX: The join clause is parse into tokens delimited by "AND"
            the special case of "BETWEEN x AND y" overrides the "AND" delimter
           These tokens are then parses as "relations"
   NOTE: Joins currently work only on a single index and NOT on a star schema */
//TODO (parseWCReply & joinParseWCReply) are identical -> (refactor + merge)
static bool joinParseWCReply(redisClient  *c, jb_t *jb) {
    cswc_t *w      = &jb->w;
    w->wtype       = SQL_ERR_LOOKUP;
    uchar   sop    = SQL_SELECT;
    char   *finish = jb->w.token;
    int     ntoks  = 0;
    sds     token  = NULL;
    bool    ret    = 0;

    while (1) {
        f_t   flt;
        initFilter(&flt);
        //TODO needs to be str_case_unescaped_quotes_str(" AND ")
        char *tokfin = NULL;
        char *and    = strcasestr(finish, " AND ");
        char *btwn   = strcasestr(finish, " BETWEEN ");
        if (and && btwn && btwn < and) and = strcasestr(and + 5, " AND ");
        int   tlen   = and ? and - finish : (int)strlen(finish);
        if (token) sdsfree(token);
        token        = sdsnewlen(finish, tlen);
        w->wtype     = parseWCTokenRelation(c, w, sop, token, &tokfin,
                                            0, &flt, 0, 1);
        if (w->wtype == SQL_ERR_LOOKUP) goto j_pcw_end;

        if (!addInd2Join(c, w->imatch, &jb->n_ind, jb->j_indxs)) goto j_pcw_end;

        sds key = w->key ? w->key->ptr : NULL;
        if (key) { /* parsed key may be "tablename.columnname" i.e. join_indx */
            if (isalpha(*key) && strchr(key, '.')) {
                if (!parseInumTblCol(c, &w->tmatch, key, sdslen(key),
                                    &w->cmatch, &w->imatch, 0)) goto j_pcw_end;
                if (!addInd2Join(c, w->imatch,
                                 &jb->n_ind, jb->j_indxs))       goto j_pcw_end;
                decrRefCount(w->key);
                w->key = NULL;
           } else { /* or if it is a key, it currently needs to be a range */
               singleFKHack(w, &w->wtype);
           }
        }
        ntoks++;

        if (and) {
            finish = and + 5; /* go past " AND " */
            SKIP_SPACES(finish)
            if (!*finish) break;
        } else {
            if (!tokfin) break;
            finish = tokfin;
            SKIP_SPACES(finish)
            if (!*finish) break;
            if (!parseWCAddtlSQL(c, finish, w)) goto j_pcw_end;
            break;
        }
    }
    if (w->lvr) { /* leftover from parsing */
        SKIP_SPACES(w->lvr)
        if (*(w->lvr)) goto j_pcw_end;
    }

    if (w->obc[0] != -1 ) { /* ORDER BY -> Table must be in join */
        bool hit = 0;
        for (int i = 0; i < jb->qcols; i++) {
            if (jb->j_tbls[i] == w->obt[0]) {
                hit = 1;
                break;
            }
        }
        if (!hit) {
            addReply(c, shared.join_table_not_in_query);
            goto j_pcw_end;
        }
    }
    if (jb->n_ind == 0) {
        addReply(c, shared.joinindexedcolumnlisterror);
        goto j_pcw_end;
    }
    if (jb->n_ind < 2) {
        addReply(c, shared.toofewindicesinjoin);
        goto j_pcw_end;
    }
    if (!w->low && !w->inl) {
        addReply(c, shared.join_requires_range);
        goto j_pcw_end;
    }
    if (jb->n_ind > ntoks) {
        addReply(c, shared.join_on_multi_col);
        goto j_pcw_end;
    }

    ret = 1;
j_pcw_end:
    if (w->lvr)  w->lvr  = sdsnewlen(w->lvr, strlen(w->lvr));
    if (token) sdsfree(token);
    return ret;
}

void init_join_block(jb_t *jb, char *wc) {
    init_check_sql_where_clause(&(jb->w), -1, wc);
    jb->n_ind = 0;
    jb->qcols = 0;
    jb->cstar = 0;
}
void destroy_join_block(jb_t *jb) {
    destroy_check_sql_where_clause(&(jb->w));
}

bool parseJoinReply(redisClient *c,
                    jb_t        *jb,
                    char        *clist,
                    char        *tlist) {
    int  tmatchs[MAX_JOIN_INDXS];
    bool bdum;
    int  numt = 0;

    /* Check tbls in "FROM tbls,,,," */
    if (!parseCommaSpaceListReply(c, tlist, 0, 1, 0, -1, NULL,
                                  &numt, tmatchs, NULL, NULL,
                                  NULL, &bdum)) return 0;

    /* Check all tbl.cols in "SELECT tbl1.col1, tbl2.col2,,,,," */
    if (!parseCommaSpaceListReply(c, clist, 0, 0, 1, -1, NULL,
                                  &numt, tmatchs, jb->j_tbls, jb->j_cols,
                                  &jb->qcols, &jb->cstar)) return 0;

    bool ret = joinParseWCReply(c, jb);
    if (!leftoverParsingReply(c, jb->w.lvr)) return 0;
    return ret;
}

void joinReply(redisClient *c) {
    jb_t jb;
    init_join_block(&jb, c->argv[5]->ptr);
    if (!parseJoinReply(c, &jb, c->argv[1]->ptr, c->argv[3]->ptr)) goto j_end;

    if (jb.w.wtype > SQL_STORE_LOOKUP_MASK) {
        jb.w.wtype -= SQL_STORE_LOOKUP_MASK;
        if (!jb.w.low && !jb.w.inl) {
            addReply(c, shared.selectsyntax_store_norange);
            goto j_end;
        } else if (jb.cstar) {
            addReply(c, shared.select_store_count);
            goto j_end;
        }
        if (server.maxmemory && zmalloc_used_memory() > server.maxmemory) {
            addReplySds(c, sdsnew(
                "-ERR command not allowed when used memory > 'maxmemory'\r\n"));
            goto j_end;
        }
        luaIstoreCommit(c);
    } else {
        if (jb.cstar) { /* SELECT PK per index - minimum cols for "COUNT(*)" */
           jb.qcols = 0;
           for (int i = 0; i < jb.n_ind; i++) {
               jb.j_tbls[i] = Index[server.dbid][jb.j_indxs[i]].table;
               jb.j_cols[i] = 0; /* PK */
               jb.qcols++;
           }
        }
        joinGeneric(c, &jb);
    }

j_end:
    destroy_join_block(&jb);
}
