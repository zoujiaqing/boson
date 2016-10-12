#ifndef BOSON_QUEUES_WFQUEUE_H_
#define BOSON_QUEUES_WFQUEUE_H_
#include <unistd.h>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <list>
#include <type_traits>
#include <utility>
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-security"
#pragma GCC diagnostic pop

#include "wfqueue/align.h"
#include "wfqueue/primitives.h"
#define EMPTY ((void *)0)
#ifndef WFQUEUE_NODE_SIZE
#define WFQUEUE_NODE_SIZE ((1 << 10) - 2)
#endif
//#define N WFQUEUE_NODE_SIZE
#define BOT ((void *)0)
#define TOP ((void *)-1)
#define MAX_GARBAGE(n) (2 * n)
#ifndef MAX_SPIN
#define MAX_SPIN 100
#endif
#ifndef MAX_PATIENCE
#define MAX_PATIENCE 10
#endif

namespace boson {
namespace queues {
/**
 * Wfqueue is a wait-free MPMC concurrent queue
 *
 * @see https://github.com/chaoran/fast-wait-free-queue
 *
 * Algorithm from Chaoran Yang and John Mellor-Crummey
 * Currently, thei algorithms bugs here so we use LCRQ
 */
class base_wfqueue {
  //////////////

  struct _enq_t {
    std::atomic<long> id;
    std::atomic<void *> val;
  } CACHE_ALIGNED;

  struct _deq_t {
    std::atomic<long> id;
    std::atomic<long> idx;
  } CACHE_ALIGNED;

  struct _cell_t {
    std::atomic<void *> val;
    std::atomic<struct _enq_t *> enq;
    std::atomic<struct _deq_t *> deq;
    void *pad[5];
  };

  struct _node_t {
    std::atomic<struct _node_t *> next CACHE_ALIGNED;
    long id CACHE_ALIGNED;
    struct _cell_t cells[WFQUEUE_NODE_SIZE] CACHE_ALIGNED;
  };

  typedef struct DOUBLE_CACHE_ALIGNED {
    /**
     * Index of the next position for enqueue.
     */
    std::atomic<long> Ei DOUBLE_CACHE_ALIGNED;

    /**
     * Index of the next position for dequeue.
     */
    std::atomic<long> Di DOUBLE_CACHE_ALIGNED;

    /**
     * Index of the head of the queue.
     */
    std::atomic<long> Hi DOUBLE_CACHE_ALIGNED;

    /**
     * Pointer to the head node of the queue.
     */
    std::atomic<struct _node_t *> Hp;

    /**
     * Number of processors.
     */
    long nprocs;
#ifdef RECORD
    long slowenq;
    long slowdeq;
    long fastenq;
    long fastdeq;
    long empty;
#endif
  } queue_t;

  typedef struct _handle_t {
    /**
     * Pointer to the next handle.
     */
    std::atomic<struct _handle_t *> next;

    /**
     * Hazard pointer.
     */
    std::atomic<struct _node_t *> Hp;

    /**
     * Pointer to the node for enqueue.
     */
    std::atomic<struct _node_t *> Ep;

    /**
     * Pointer to the node for dequeue.
     */
    std::atomic<struct _node_t *> Dp;

    /**
     * Enqueue request.
     */
    struct _enq_t Er CACHE_ALIGNED;

    /**
     * Dequeue request.
     */
    struct _deq_t Dr CACHE_ALIGNED;

    /**
     * Handle of the next enqueuer to help.
     */
    struct _handle_t *Eh CACHE_ALIGNED;

    long Ei;

    /**
     * Handle of the next dequeuer to help.
     */
    struct _handle_t *Dh;

    /**
     * Pointer to a spare node to use, to speedup adding a new node.
     */
    struct _node_t *spare CACHE_ALIGNED;

    /**
     * Count the delay rounds of helping another dequeuer.
     */
    int delay;

#ifdef RECORD
    long slowenq;
    long slowdeq;
    long fastenq;
    long fastdeq;
    long empty;
#endif
  } handle_t;

  typedef struct _enq_t enq_t;
  typedef struct _deq_t deq_t;
  typedef struct _cell_t cell_t;
  typedef struct _node_t node_t;

  inline void *spin(std::atomic<void *> *p) {
    int patience = MAX_SPIN;
    void *v = *p;

    while (!v && patience-- > 0) {
      v = *p;
      PAUSE();
    }

    return v;
  }

  inline node_t *new_node() {
    node_t *n = (node_t *)align_malloc(PAGE_SIZE, sizeof(node_t));
    memset(n, 0, sizeof(node_t));
    return n;
  }

  node_t *update(std::atomic<node_t *> *pPn, node_t *cur, std::atomic<node_t *> *pHp) {
    node_t *ptr = *pPn;

    if (ptr->id < cur->id) {
      if (!CAScs(pPn, &ptr, cur)) {
        if (ptr->id < cur->id) cur = ptr;
      }

      node_t *Hp = *pHp;
      if (Hp && Hp->id < cur->id) cur = Hp;
    }

    return cur;
  }

  void cleanup(queue_t *q, handle_t *th) {
    long oid = q->Hi;
    node_t *newnode = th->Dp;

    if (oid == -1) return;
    if (newnode->id - oid < MAX_GARBAGE(q->nprocs)) return;
    if (!q->Hi.compare_exchange_strong(oid, -1, std::memory_order_acquire,
                                       std::memory_order_relaxed))
      return;

    node_t *old = q->Hp;
    handle_t *ph = th;
    handle_t *phs[q->nprocs];
    int i = 0;

    do {
      node_t *Hp = ph->Hp.load(std::memory_order_acquire);
      if (Hp && Hp->id < newnode->id) newnode = Hp;

      newnode = update(&ph->Ep, newnode, &ph->Hp);
      newnode = update(&ph->Dp, newnode, &ph->Hp);

      phs[i++] = ph;
      ph = ph->next;
    } while (newnode->id > oid && ph != th);

    while (newnode->id > oid && --i >= 0) {
      node_t *Hp = phs[i]->Hp.load(std::memory_order_acquire);
      if (Hp && Hp->id < newnode->id) newnode = Hp;
    }

    long nid = newnode->id;

    if (nid <= oid) {
      q->Hi.store(oid, std::memory_order_release);
    } else {
      q->Hp = newnode;
      q->Hi.store(nid, std::memory_order_release);

      while (old != newnode) {
        node_t *tmp = old->next;
        free(old);
        old = tmp;
      }
    }
  }

  cell_t *find_cell(std::atomic<node_t *> *ptr, long i, handle_t *th) {
    node_t *curr = *ptr;

    long j;
    for (j = curr->id; j < i / WFQUEUE_NODE_SIZE; ++j) {
      node_t *next = curr->next;

      if (next == NULL) {
        node_t *temp = th->spare;

        if (!temp) {
          temp = new_node();
          th->spare = temp;
        }

        temp->id = j + 1;

        if (curr->next.compare_exchange_strong(next, temp, std::memory_order_release,
                                               std::memory_order_acquire)) {
          next = temp;
          th->spare = NULL;
        }
      }

      curr = next;
    }

    *ptr = curr;
    return &curr->cells[i % WFQUEUE_NODE_SIZE];
  }

  int enq_fast(queue_t *q, handle_t *th, void *v, long *id) {
    long i = q->Ei.fetch_add(1, std::memory_order_seq_cst);
    cell_t *c = find_cell(&th->Ep, i, th);
    void *cv = BOT;

    if (CAS(&c->val, &cv, v)) {
#ifdef RECORD
      th->fastenq++;
#endif
      return 1;
    } else {
      *id = i;
      return 0;
    }
  }

  void enq_slow(queue_t *q, handle_t *th, void *v, long id) {
    enq_t *enq = &th->Er;
    enq->val = v;
    RELEASE(&enq->id, id);

    std::atomic<node_t *> tail{th->Ep.load()};
    long i;
    cell_t *c;

    do {
      i = q->Ei.fetch_add(1);
      c = find_cell(&tail, i, th);
      enq_t *ce = (enq_t *)BOT;

      if (c->enq.compare_exchange_strong(ce, enq, std::memory_order_seq_cst,
                                         std::memory_order_seq_cst) &&
          c->val != TOP) {
        if (enq->id.compare_exchange_strong(id, -i, std::memory_order_relaxed,
                                            std::memory_order_relaxed))
          id = -i;
        break;
      }
    } while (enq->id > 0);

    id = -enq->id;
    c = find_cell(&th->Ep, id, th);
    if (id > i) {
      long Ei = q->Ei;
      while (Ei <= id && !CAS(&q->Ei, &Ei, id + 1))
        ;
    }
    c->val = v;

#ifdef RECORD
    th->slowenq++;
#endif
  }

  void enqueue(queue_t *q, handle_t *th, void *v) {
    th->Hp.store(th->Ep);

    long id;
    int p = MAX_PATIENCE;
    while (!enq_fast(q, th, v, &id) && p-- > 0)
      ;
    if (p < 0) enq_slow(q, th, v, id);

    RELEASE(&th->Hp, NULL);
  }

  void *help_enq(queue_t *q, handle_t *th, cell_t *c, long i) {
    void *v = spin(&c->val);

    if ((v != TOP && v != BOT) || (v == BOT && !CAScs(&c->val, &v, TOP) && v != TOP)) {
      return v;
    }

    enq_t *e = c->enq;

    if (e == BOT) {
      handle_t *ph;
      enq_t *pe;
      long id;
      ph = th->Eh, pe = &ph->Er, id = pe->id;

      if (th->Ei != 0 && th->Ei != id) {
        th->Ei = 0;
        th->Eh = ph->next;
        ph = th->Eh, pe = &ph->Er, id = pe->id;
      }

      if (id > 0 && id <= i && !CAS(&c->enq, &e, pe))
        th->Ei = id;
      else
        th->Eh = ph->next;

      if (e == BOT && CAS(&c->enq, &e, TOP)) e = (enq_t *)TOP;
    }

    if (e == TOP) return (q->Ei <= i ? BOT : TOP);

    long ei = e->id.load(std::memory_order_acquire);
    void *ev = e->val.load(std::memory_order_acquire);

    if (ei > i) {
      if (c->val == TOP && q->Ei <= i) return BOT;
    } else {
      if ((ei > 0 && CAS(&e->id, &ei, -i)) || (ei == -i && c->val == TOP)) {
        long Ei = q->Ei;
        while (Ei <= i && !CAS(&q->Ei, &Ei, i + 1))
          ;
        c->val = ev;
      }
    }

    return c->val;
  }

  void help_deq(queue_t *q, handle_t *th, handle_t *ph) {
    deq_t *deq = &ph->Dr;
    long idx = deq->idx.load(std::memory_order_acquire);
    long id = deq->id;

    if (idx < id) return;

    std::atomic<node_t *> Dp{ph->Dp.load()};
    th->Hp = Dp.load();
    FENCE();
    idx = deq->idx;

    long i = id + 1, old = id, newid = 0;
    while (1) {
      std::atomic<node_t *> h{Dp.load()};
      for (; idx == old && newid == 0; ++i) {
        cell_t *c = find_cell(&h, i, th);

        long Di = q->Di;
        while (Di <= i && !CAS(&q->Di, &Di, i + 1))
          ;

        void *v = help_enq(q, th, c, i);
        if (v == BOT || (v != TOP && c->deq == BOT))
          newid = i;
        else
          idx = deq->idx.load(std::memory_order_acquire);
      }

      if (newid != 0) {
        if (CASra(&deq->idx, &idx, newid)) idx = newid;
        if (idx >= newid) newid = 0;
      }

      if (idx < 0 || deq->id != id) break;

      cell_t *c = find_cell(&Dp, idx, th);
      deq_t *cd = (deq_t *)BOT;
      if (c->val == TOP || CAS(&c->deq, &cd, deq) || cd == deq) {
        CAS(&deq->idx, &idx, -idx);
        break;
      }

      old = idx;
      if (idx >= i) i = idx + 1;
    }
  }

  void *deq_fast(queue_t *q, handle_t *th, long *id) {
    long i = q->Di.fetch_add(1, std::memory_order_seq_cst);
    cell_t *c = find_cell(&th->Dp, i, th);
    void *v = help_enq(q, th, c, i);
    deq_t *cd = (deq_t *)BOT;

    if (v == BOT) return BOT;
    if (v != TOP && CAS(&c->deq, &cd, TOP)) return v;

    *id = i;
    return TOP;
  }

  void *deq_slow(queue_t *q, handle_t *th, long id) {
    deq_t *deq = &th->Dr;
    RELEASE(&deq->id, id);
    RELEASE(&deq->idx, id);

    help_deq(q, th, th);
    long i = -deq->idx;
    cell_t *c = find_cell(&th->Dp, i, th);
    void *val = c->val;

#ifdef RECORD
    th->slowdeq++;
#endif
    return val == TOP ? BOT : val;
  }

  void *dequeue(queue_t *q, handle_t *th) {
    th->Hp.store(th->Dp);

    void *v;
    long id;
    int p = MAX_PATIENCE;

    do
      v = deq_fast(q, th, &id);
    while (v == TOP && p-- > 0);
    if (v == TOP)
      v = deq_slow(q, th, id);
    else {
#ifdef RECORD
      th->fastdeq++;
#endif
    }

    if (v != EMPTY) {
      help_deq(q, th, th->Dh);
      th->Dh = th->Dh->next;
    }

    RELEASE(&th->Hp, NULL);

    if (th->spare == NULL) {
      cleanup(q, th);
      th->spare = new_node();
    }

#ifdef RECORD
    if (v == EMPTY) th->empty++;
#endif
    return v;
  }

  pthread_barrier_t barrier;

  void queue_init(queue_t *q, int nprocs) {
    q->Hi = 0;
    q->Hp = new_node();

    q->Ei = 1;
    q->Di = 1;

    q->nprocs = nprocs;

#ifdef RECORD
    q->fastenq = 0;
    q->slowenq = 0;
    q->fastdeq = 0;
    q->slowdeq = 0;
    q->empty = 0;
#endif
    // pthread_barrier_init(&barrier, NULL, nprocs);
  }

  void queue_free(queue_t *q, handle_t *h) {
#ifdef RECORD
    int lock = 0;

    FAA(&q->fastenq, h->fastenq);
    FAA(&q->slowenq, h->slowenq);
    FAA(&q->fastdeq, h->fastdeq);
    FAA(&q->slowdeq, h->slowdeq);
    FAA(&q->empty, h->empty);

    pthread_barrier_wait(&barrier);

    if (FAA(&lock, 1) == 0)
      printf("Enq: %f Deq: %f Empty: %f\n", q->slowenq * 100.0 / (q->fastenq + q->slowenq),
             q->slowdeq * 100.0 / (q->fastdeq + q->slowdeq),
             q->empty * 100.0 / (q->fastdeq + q->slowdeq));
#endif
  }

  void queue_register(queue_t *q, handle_t *th, int id) {
    th->next = NULL;
    th->Hp = NULL;
    th->Ep.store(q->Hp);
    th->Dp.store(q->Hp);

    th->Er.id = 0;
    th->Er.val = BOT;
    th->Dr.id = 0;
    th->Dr.idx = -1;

    th->Ei = 0;
    th->spare = new_node();
#ifdef RECORD
    th->slowenq = 0;
    th->slowdeq = 0;
    th->fastenq = 0;
    th->fastdeq = 0;
    th->empty = 0;
#endif

    handle_t *tail = _tail.load();

    if (tail == nullptr) {
      th->next = th;
      if (_tail.compare_exchange_strong(tail, th, std::memory_order_release,
                                        std::memory_order_acquire)) {
        th->Eh = th->next;
        th->Dh = th->next;
        return;
      }
    }

    handle_t *next = tail->next;
    do
      th->next = next;
    while (!tail->next.compare_exchange_strong(next, th, std::memory_order_release,
                                               std::memory_order_acquire));

    th->Eh = th->next;
    th->Dh = th->next;
  }

  static std::atomic<handle_t *> _tail;

  ////////////

  queue_t *queue_;
  handle_t **hds_;
  int nprocs_;

  handle_t *get_handle(std::size_t proc_id);

 public:
  base_wfqueue(int nprocs);
  base_wfqueue(base_wfqueue const &) = delete;
  base_wfqueue(base_wfqueue &&) = default;
  base_wfqueue &operator=(base_wfqueue const &) = delete;
  base_wfqueue &operator=(base_wfqueue &&) = default;
  ~base_wfqueue();
  void write(std::size_t proc_id, void *data);
  void *read(std::size_t proc_id);
};

};  // namespace queues
};  // namespace boson

#endif  // BOSON_QUEUES_WFQUEUE_H_
