/*-
 * Copyright 1997, 1998, 2001 John-Mark Gurney.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 *	$Id: btree.h,v 1.4.2.1 2001/03/28 06:17:12 jmg Exp $
 *
 */

#ifndef _BTREE_H_
#define _BTREE_H_

struct btree;
struct btreenode;

#define	BT_SIZEDEF	128

//#define VOIDSIZE sizeof(void *) /* UU would not work on 32bit */
#define VOIDSIZE 8
#define UINTSIZE sizeof(unsigned int)

typedef void * bt_data_t;
typedef int (*bt_cmp_t)(bt_data_t k1, bt_data_t k2);

struct btree *bt_create(bt_cmp_t      cmp,
                        unsigned char trans,
                        int           keysize,
                        unsigned char ainc);
void *bt_malloc(int size, struct btree *btr);
void  bt_free(void *v, struct btree *btr, int size);
void  bt_free_btreenode(struct btreenode *x, struct btree *btr);
void  bt_free_btree(void *obtr, struct btree *btr);

void  bt_insert(struct btree *btr, bt_data_t k);
void *bt_delete(struct btree *btr, bt_data_t k);
void *bt_replace(struct btree *btr, bt_data_t k, bt_data_t val);

void bt_dump_info(struct btree *btr);
void  bt_dumptree(struct btree *btr);
void  bt_treestats(struct btree *btr);
int   bt_checktree(struct btree *btr, bt_data_t kmin, bt_data_t kmax);

void *bt_max(struct btree *btr);
void *bt_min(struct btree *btr);
void *bt_find(struct btree *btr, bt_data_t k);

struct btIterator;
struct btreenode;
int  bt_init_iterator(struct btree *bre, bt_data_t k, struct btIterator *iter);
int  bt_find_closest_slot(struct btree *btr, struct btreenode *x, bt_data_t k);

void dump_bt_mem_profile(struct btree *btr);
#endif /* _BTREE_H_ */
