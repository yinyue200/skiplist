/**
 * Copyright (C) 2017-present Jung-Sang Ahn <jungsang.ahn@gmail.com>
 * All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use,
 * copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following
 * conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include <stdlib.h>

#include "skiplist.h"

//#define __SL_DEBUG
#ifdef __SL_DEBUG
    #include "skiplist_debug.h"
#else
    #define __SLD_RT_INS(e, n, t, c)
    #define __SLD_NC_INS(n, nn, t, c)
    #define __SLD_RT_RMV(e, n, t, c)
    #define __SLD_NC_RMV(n, nn, t, c)
    #define __SLD_BM(n)
    #define __SLD_ASSERT(cond)
#endif

#ifdef STL_ATOMIC
    // C++ (STL) atomic operations
    #define MOR std::memory_order_relaxed
    #define ATM_LOAD(var, val) val = (var).load(MOR)
    #define ATM_STORE(var, val) (var).store(val, MOR)
    #define ATM_CAS(var, exp, val) (var).compare_exchange_weak(exp, val)
#else
    // C-style atomic operations
    #define MOR __ATOMIC_RELAXED
    #define ATM_LOAD(var, val) __atomic_load(&var, &val, MOR)
    #define ATM_STORE(var, val) __atomic_store(&var, &val, MOR)
    #define ATM_CAS(var, exp, val) \
        __atomic_compare_exchange(&var, &exp, &val, 1, MOR, MOR)
#endif

static inline void _sl_node_init(SkiplistNode *node,
                                 size_t top_layer)
{
    if (top_layer > UINT8_MAX) {
        top_layer = UINT8_MAX;
    }

    bool bool_val = false;
    ATM_STORE(node->isFullyLinked, bool_val);
    ATM_STORE(node->beingModified, bool_val);
    ATM_STORE(node->removed, bool_val);

    if (node->topLayer != top_layer ||
        node->next == NULL) {

        node->topLayer = top_layer;

        if (node->next) {
            delete[] node->next;
        }
        node->next = new atm_node_ptr[top_layer+1];
    }
}

void skiplist_init(SkiplistRaw *slist,
                   skiplist_cmp_t *cmp_func) {
    _sl_node_init(&slist->head, slist->maxLayer);
    _sl_node_init(&slist->tail, slist->maxLayer);

    size_t layer;
    for (layer = 0; layer < slist->maxLayer; ++layer) {
        slist->head.next[layer] = &slist->tail;
        slist->tail.next[layer] = NULL;
    }

    bool bool_val = true;
    ATM_STORE(slist->head.isFullyLinked, bool_val);
    ATM_STORE(slist->tail.isFullyLinked, bool_val);
    slist->cmpFunc = cmp_func;
}

void skiplist_set_config(SkiplistRaw *slist,
                         SkiplistRawConfig config)
{
    slist->fanout = config.fanout;
    slist->maxLayer = config.maxLayer;
    slist->aux = config.aux;
}

SkiplistRawConfig skiplist_get_config(SkiplistRaw *slist)
{
    SkiplistRawConfig ret;
    ret.fanout = slist->fanout;
    ret.maxLayer = slist->maxLayer;
    ret.aux = slist->aux;
    return ret;
}

static inline int _sl_cmp(SkiplistRaw *slist,
                          SkiplistNode *a,
                          SkiplistNode *b)
{
    if (a == b) {
        return 0;
    }
    if (a == &slist->head ||
        b == &slist->tail) {
        return -1;
    }
    if (a == &slist->tail ||
        b == &slist->head) {
        return 1;
    }
    return slist->cmpFunc(a, b, slist->aux);
}

static inline bool _sl_valid_node(SkiplistNode *node) {
    bool removed = false;
    bool is_fully_linked = false;

    ATM_LOAD(node->removed, removed);
    ATM_LOAD(node->isFullyLinked, is_fully_linked);

    return !removed && is_fully_linked;
}

static inline SkiplistNode* _sl_next(SkiplistRaw *slist,
                                     SkiplistNode *cur_node,
                                     int layer)
{
    SkiplistNode *next_node = NULL;
    ATM_LOAD(cur_node->next[layer], next_node);
    while ( next_node && !_sl_valid_node(next_node) ) {
        ATM_LOAD(next_node->next[layer], next_node);
    }
    return next_node;
}

static inline size_t _sl_decide_top_layer(SkiplistRaw *slist)
{
    size_t layer = 0;
    while (layer+1 < slist->maxLayer) {
        // coin filp
        if (rand() % slist->fanout == 0) {
            // grow: 1/fanout probability
            layer++;
        } else {
            // stop: 1 - 1/fanout probability
            break;
        }
    }
    return layer;
}

static inline void _sl_clr_flags(SkiplistNode** node_arr,
                                 int start_layer,
                                 int top_layer)
{
    int layer;
    for (layer = start_layer; layer <= top_layer; ++layer) {
        if ( layer == top_layer ||
             node_arr[layer] != node_arr[layer+1] ) {

            bool being_modified = false;
            ATM_LOAD(node_arr[layer]->beingModified, being_modified);
            __SLD_ASSERT(being_modified == true);
            (void)being_modified;

            bool bool_val = false;
            ATM_STORE(node_arr[layer]->beingModified, bool_val);
        }
    }
}

static inline bool _sl_valid_prev_next(SkiplistNode *prev,
                                       SkiplistNode *next) {
    return _sl_valid_node(prev) && _sl_valid_node(next);
}

void skiplist_insert(SkiplistRaw *slist,
                     SkiplistNode *node)
{
    int top_layer = _sl_decide_top_layer(slist);
    bool bool_val = true;

    // init node before insertion
    _sl_node_init(node, top_layer);

    SkiplistNode* prevs[SKIPLIST_MAX_LAYER];
    SkiplistNode* nexts[SKIPLIST_MAX_LAYER];

insert_retry:
    int cmp = 0;
    int cur_layer = 0;
    int layer;
    SkiplistNode *cur_node = &slist->head;

    for (cur_layer = slist->maxLayer-1; cur_layer >= 0; --cur_layer) {
        do {
            SkiplistNode *next_node = _sl_next(slist, cur_node, cur_layer);
            cmp = _sl_cmp(slist, node, next_node);
            if (cmp > 0) {
                // next_node < node
                // => move to next node
                cur_node = next_node;
                continue;
            }
            // otherwise: node <= next_node

            if (cur_layer <= top_layer) {
                prevs[cur_layer] = cur_node;
                nexts[cur_layer] = next_node;

                // both 'prev' and 'next' should be fully linked before
                // insertion, and no other thread should not modify 'prev'
                // at the same time.

                int error_code = 0;
                int locked_layer = cur_layer + 1;

                // check if prev node is duplicated with upper layer
                if (cur_layer < top_layer &&
                    prevs[cur_layer] == prevs[cur_layer+1]) {
                    // duplicate
                    // => which means that 'beingModified' flag is already true
                    // => do nothing
                } else {
                    bool expected = false;
                    bool_val = true;
                    if (ATM_CAS(prevs[cur_layer]->beingModified, expected, bool_val)) {
                        locked_layer = cur_layer;
                    } else {
                        error_code = -1;
                    }
                }

                if (error_code == 0 &&
                    !_sl_valid_prev_next(prevs[cur_layer], nexts[cur_layer])) {
                    error_code = -2;
                }

                if (error_code != 0) {
                    __SLD_RT_INS(error_code, node, top_layer, cur_layer);
                    _sl_clr_flags(prevs, locked_layer, top_layer);
                    goto insert_retry;
                }

                // set current node's pointers
                ATM_STORE(node->next[cur_layer], nexts[cur_layer]);

                if (_sl_next(slist, cur_node, cur_layer) != next_node) {
                    __SLD_NC_INS(cur_node, next_node, top_layer, cur_layer);
                    // clear including the current layer
                    // as we already set modification flag above.
                    _sl_clr_flags(prevs, cur_layer, top_layer);
                    goto insert_retry;
                }
            }

            if (cur_layer) {
                // non-bottom layer => go down
                break;
            }

            // bottom layer => insertion succeeded
            // change prev/next nodes' prev/next pointers from 0 ~ top_layer
            for (layer = 0; layer <= top_layer; ++layer) {
                ATM_STORE(prevs[layer]->next[layer], node);
            }

            // now this node is fully linked
            bool_val = true;
            ATM_STORE(node->isFullyLinked, bool_val);

            // modification is done for all layers
            _sl_clr_flags(prevs, 0, top_layer);
            return;

        } while (cur_node != &slist->tail);
    }
}

SkiplistNode* skiplist_find(SkiplistRaw *slist,
                            SkiplistNode *query)
{
    int cmp = 0;
    int cur_layer = 0;
    SkiplistNode *cur_node = &slist->head;

    for (cur_layer = slist->maxLayer-1; cur_layer >= 0; --cur_layer) {
        do {
            SkiplistNode *next_node = _sl_next(slist, cur_node, cur_layer);
            cmp = _sl_cmp(slist, query, next_node);
            if (cmp > 0) {
                // next_node < query
                // => move to next node
                cur_node = next_node;
                continue;
            } else if (cmp == 0) {
                // query == next_node .. return
                return next_node;
            }

            // query < next_node
            if (cur_layer) {
                // non-bottom layer => go down
                break;
            }

            // bottom layer => means that exact match doesn't exist.
            return NULL;

        } while (cur_node != &slist->tail);
    }

    return NULL;
}

SkiplistNode* skiplist_find_smaller(SkiplistRaw *slist,
                                    SkiplistNode *query)
{
    int cmp = 0;
    int cur_layer = 0;
    SkiplistNode *cur_node = &slist->head;

    for (cur_layer = slist->maxLayer-1; cur_layer >= 0; --cur_layer) {
        do {
            SkiplistNode *next_node = _sl_next(slist, cur_node, cur_layer);
            cmp = _sl_cmp(slist, query, next_node);
            if (cmp > 0) {
                // next_node < query
                // => move to next node
                cur_node = next_node;
                continue;
            }

            // otherwise: query <= next_node
            if (cur_layer) {
                // non-bottom layer => go down
                break;
            }

            // bottom layer => return cur_node
            return cur_node;

        } while (cur_node != &slist->tail);
    }

    return NULL;
}

int skiplist_erase_node(SkiplistRaw *slist,
                        SkiplistNode *node)
{
    int top_layer = node->topLayer;
    bool bool_val = true;
    bool removed = false;
    bool is_fully_linked = false;

    ATM_LOAD(node->removed, removed);
    if (removed) {
        // already removed
        return -1;
    }

    SkiplistNode* prevs[SKIPLIST_MAX_LAYER];
    SkiplistNode* nexts[SKIPLIST_MAX_LAYER];

    bool expected = false;
    bool_val = true;
    if (!ATM_CAS(node->beingModified, expected, bool_val)) {
        // already being modified .. fail
        __SLD_BM(node);
        return -2;
    }

    // clear removed flag first, so that reader cannot read this node.
    bool_val = true;
    ATM_STORE(node->removed, bool_val);

erase_node_retry:
    ATM_LOAD(node->isFullyLinked, is_fully_linked);
    if (!is_fully_linked) {
        // already unlinked .. remove is done by other thread
        return -3;
    }

    int cmp = 0;
    int cur_layer = slist->maxLayer - 1;
    SkiplistNode *cur_node = &slist->head;

    for (; cur_layer >= 0; --cur_layer) {
        do {
            SkiplistNode *next_node = _sl_next(slist, cur_node, cur_layer);

            cmp = _sl_cmp(slist, node, next_node);
            if (cmp > 0) {
                // 'next_node' < 'node'
                // => move to next node
                cur_node = next_node;
                continue;
            }
            // otherwise: 'node' <= 'next_node'

            if (cur_layer <= top_layer) {
                prevs[cur_layer] = cur_node;
                // note: 'next_node' should not be 'node',
                //       as 'removed' flag is already set.
                __SLD_ASSERT(next_node != node);
                nexts[cur_layer] = next_node;

                // check if prev node duplicates with upper layer
                int error_code = 0;
                int locked_layer = cur_layer + 1;
                if (cur_layer < top_layer &&
                    prevs[cur_layer] == prevs[cur_layer+1]) {
                    // duplicate with upper layer
                    // => which means that 'beingModified' flag is already true
                    // => do nothing.
                } else {
                    expected = false;
                    bool_val = true;
                    if (ATM_CAS(prevs[cur_layer]->beingModified, expected, bool_val)) {
                        locked_layer = cur_layer;
                    } else {
                        error_code = -1;
                    }
                }

                if (error_code == 0 &&
                    !_sl_valid_prev_next(prevs[cur_layer], nexts[cur_layer])) {
                    error_code = -2;
                }

                if (error_code != 0) {
                    __SLD_RT_RMV(error_code, node, top_layer, cur_layer);
                    _sl_clr_flags(prevs, locked_layer, top_layer);
                    goto erase_node_retry;
                }

                if (_sl_next(slist, cur_node, cur_layer) != nexts[cur_layer]) {
                    __SLD_NC_RMV(cur_node, nexts[cur_layer], top_layer, cur_layer);
                    _sl_clr_flags(prevs, cur_layer, top_layer);
                    goto erase_node_retry;
                }
            }

            // go down
            break;

        } while (cur_node != &slist->tail);
    }

    // bottom layer => removal succeeded.
    // change prev nodes' next pointer from 0 ~ top_layer
    for (cur_layer = 0; cur_layer <= top_layer; ++cur_layer) {
        ATM_STORE(prevs[cur_layer]->next[cur_layer], nexts[cur_layer]);
    }

    // now this node is unlinked
    bool_val = false;
    ATM_STORE(node->isFullyLinked, bool_val);

    // modification is done for all layers
    _sl_clr_flags(prevs, 0, top_layer);

    bool_val = false;
    ATM_STORE(node->beingModified, bool_val);
    return 0;
}

int skiplist_erase(SkiplistRaw *slist,
                   SkiplistNode *query)
{
    SkiplistNode *found = skiplist_find(slist, query);
    if (!found) {
        // key not found
        return -4;
    }

    int ret = 0;
    do {
        ret = skiplist_erase_node(slist, found);
        // if ret == -2, other thread is accessing the same node
        // at the same time. try again.
    } while (ret == -2);
    return ret;
}

SkiplistNode* skiplist_next(SkiplistRaw *slist,
                            SkiplistNode *node) {
    SkiplistNode *next = _sl_next(slist, node, 0);
    if (next == &slist->tail) {
        return NULL;
    }
    return next;
}

SkiplistNode* skiplist_prev(SkiplistRaw *slist,
                            SkiplistNode *node) {
    SkiplistNode *prev = skiplist_find_smaller(slist, node);
    if (prev == &slist->head) {
        return NULL;
    }
    return prev;
}

SkiplistNode* skiplist_begin(SkiplistRaw *slist) {
    SkiplistNode *next = _sl_next(slist, &slist->head, 0);
    if (next == &slist->tail) {
        return NULL;
    }
    return next;
}

SkiplistNode* skiplist_end(SkiplistRaw *slist) {
    return skiplist_prev(slist, &slist->tail);
}

