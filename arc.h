
/**
///    _____ ___________________  
///   /  _  \\______   \_   ___ \ 
///  /  /_\  \|       _/    \  \/ 
/// /    |    \    |   \     \____
/// \____|__  /____|_  /\______  /
///         \/       \/        \/ 
**/
///
/// @title:        ARC (Atomic Reference Counting)
/// 
/// @author:       Walker Rout
/// 
/// @contact:      walkerrout04@gmail.com
/// 
/// @date_written: 2025/01/27
/// 
/// @description:  Atomically reference counted pointers in C
/// 
/// @license:      MIT
///

/// ### Core Idea
/// A reference counted pointer can be either strong or weak:
/// - strong pointers own the allocation they point to, whereas
/// - weak pointers stake a claim on the allocation they point to..
///
/// Specifically, strong pointers manage the data at an allocation, and weak
/// pointers manage the allocation itself. By dividing the responsibilities
/// like this, we can represent all strong pointers as being backed by a single
/// weak pointer. When we free a strong, we must check if the strong count will
/// be zero, if it is, we must destroy the data we store, and free the last symbolic
/// weak the strong pointers held... if there are outstanding weaks, they will
/// preserve the allocation until the last weak is dropped, where the allocated
/// memory will finally be free'd (as it sees weak count == 0)...
///
/// Personally, i dont really like this code since i think its safety is hard to
/// reason about when you have a lot of reference counted objects, but hey its C
/// so i guess the shoe fits...
///
/// But really this does irk me, why would we delegate so much mental overhead
/// to our api consumer for such a commonly used structure/application, moreso how
/// could we avoid it? could we check for a canary value in the header? what if
/// we read uninitialized memory? can we do some pointer laundering to check the
/// data unsafely? i feel like anything that necessitates hungarian notation is
/// by nature concerning...

#ifdef __cplusplus
extern "C" {
#endif // __cplusplus

#ifndef ARC_H
#define ARC_H

#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdatomic.h>

/// Create a new strong arc pointing to data
void *arc_new(size_t nbytes);
/// Run a destructor for data when strong count is zero
void arc_free(void *arc_data, void(*destructor)(void *));
/// Clone a strong arc, incrementing strong count
void *arc_clone(void *arc_data);
/// Downgrade a strong into a weak, incrementing weak count
void *arc_downgrade(void *arc_data);

/// Free the allocation if there are no more outstanding strong arcs
void weak_free(void *weak_data);
/// Clone a weak, increment weak count, returning new weak
void *weak_clone(void *weak_data);
/// Upgrade a weak, incrementing the strong count and producing a strong arc
void *weak_upgrade(void *weak_data);

#endif // ARC_H

#ifdef ARC_IMPLEMENTATION

// this represents the header for a reference counted fat pointer
// - see http://www.schemamania.org/jkl/essays/fat-pointer.pdf or any rustlang
//   discussions for what a fat pointer is...
typedef struct arc_header {
  atomic_size_t weak_count;
  atomic_size_t strong_count;
} arc_header_t;

// statics come first...

static const size_t __ARC_ALIGN_BITS = sizeof(uintptr_t)-1;
static const size_t __ARC_HEADER_SIZE_WITH_PAD = \
  (sizeof(arc_header_t)+__ARC_ALIGN_BITS) & ~__ARC_ALIGN_BITS;

static arc_header_t *get_header(void *data) {
  // cant use void pointers with arithmetic, so we cast to a single byte type
  // to express equivalent logic...
  return (arc_header_t *)((char *) data - __ARC_HEADER_SIZE_WITH_PAD);
}

// then comes the public api...

void *arc_new(size_t nbytes) {
  if (nbytes == 0) {
    return NULL;
  }
  void *data = malloc(__ARC_HEADER_SIZE_WITH_PAD + nbytes);
  if (data == NULL) {
    return NULL;
  }
  arc_header_t *header = data;
  atomic_init(&header->strong_count, 1);
  atomic_init(&header->weak_count, 1);
  return (char *) data + __ARC_HEADER_SIZE_WITH_PAD;
}

void arc_free(void *arc_data, void(*destructor)(void *)) {
  arc_header_t *header = get_header(arc_data);
  // could use acqrel, but only the last decrement needs acquire, and others 
  // only need release (thank you mara bos)
  size_t prev = atomic_fetch_sub_explicit(&header->strong_count, 1, memory_order_release);
  // are we the last (strong) survivor?
  if (prev == 1) {
    // but first, as per 7.17.4:
    // "An atomic operation A that is a release operation on an atomic 
    // object M synchronizes with an acquire fence B if there exists some 
    // atomic operation X on M such that X is sequenced before B and reads 
    // the value written by A or a value written by any side effect in the 
    // release sequence headed by A."
    // - crucially, since fetch_sub performs a read, we can make use of an
    //   acquire fence!
    atomic_thread_fence(memory_order_acquire);
    // we have observed that we are the final surviving arc -> since we own
    // the data, we must now destroy it. delegate responsibility of freeing
    // the allocation to the weak pointer we implicitly own below...
    if (destructor != NULL) {
      destructor(arc_data);
    }
    // as noted, we now just free the symbolic weak we still hold
    void *weak_data = arc_data;
    weak_free(weak_data);
  }
}

const size_t __ARC_WEAK_MAX_REFS = SIZE_MAX>>1;

void *arc_clone(void *arc_data) {
  arc_header_t *header = get_header(arc_data);
  size_t prev = atomic_fetch_add_explicit(&header->strong_count, 1, memory_order_relaxed);
  if (prev > __ARC_WEAK_MAX_REFS-1) {
    errno = ETOOMANYREFS;
    return NULL;
  }
  return arc_data;
}

void *arc_downgrade(void *arc_data) {
  arc_header_t *header = get_header(arc_data);
  // must CAS this, since weak_count could change to 0 after we take our 
  // snapshot, so we cannot just use a fetch_add here, we could get a race
  // when the last weak pointer is dropped...
  size_t snapshot = atomic_load_explicit(&header->weak_count, memory_order_relaxed);
  for (;;) {
    // are we able to increment by 1?
    if (atomic_compare_exchange_weak_explicit(
      &header->weak_count,
      &snapshot,
      snapshot + 1,
      memory_order_relaxed,
      memory_order_relaxed
    )) {
      // yup, success, a new weak can access the memory now...
      void *weak_data = arc_data;
      return weak_data;
    }
    // we go again...
  }
}

void weak_free(void *weak_data) {
  arc_header_t *header = get_header(weak_data);
  // this part is cool; the weak manages the allocation, so we need to see
  // if there is a single weak survivor left (very likely the last arc_t)...
  // we make this operation on other threads visible through release...
  size_t prev = atomic_fetch_sub_explicit(&header->weak_count, 1, memory_order_release);
  // are we the last survivor?
  if (prev == 1) {
    // ok, lets deallocate...
    // again we establish a happens-before with the above release decrement 
    // for all modifications made ...
    atomic_thread_fence(memory_order_acquire);
    // we have exclusive access to the previously shared data, so we can 
    // free its allocation...
    free(header);
  }
}

void *weak_clone(void *weak_data) {
  arc_header_t *header = get_header(weak_data);
  size_t prev = atomic_fetch_add_explicit(&header->weak_count, 1, memory_order_relaxed);
  if (prev > __ARC_WEAK_MAX_REFS-1) {
    errno = ETOOMANYREFS;
    return NULL;
  }
  return weak_data;
}

void *weak_upgrade(void *weak_data) {
  arc_header_t *header = get_header(weak_data);
  // we must CAS in case strong_count changes... 
  size_t snapshot = atomic_load_explicit(&header->strong_count, memory_order_relaxed);
  for (;;) {
    if (snapshot == 0) {
      // we have nothing to upgrade into...
      errno = ENOENT;
      return NULL;
    }
    if (snapshot > __ARC_WEAK_MAX_REFS-1) {
      // catch overflow, same as clone...
      errno = ETOOMANYREFS;
      return NULL;
    }
    // are we able to increment by 1?
    if (atomic_compare_exchange_weak_explicit(
      &header->strong_count,
      &snapshot,
      snapshot + 1,
      // we acquire memory effects released by decrement... any thread that
      // successfully upgrades a weak can 'see' all writes made prior to
      // final drop of last strong...
      memory_order_acquire,
      memory_order_relaxed
    )) {
      // yup, success, a new arc can access the memory now...
      void *arc_data = weak_data;
      return arc_data;
    }
    // we go again...
  }
}

#endif // ARC_IMPLEMENTATION

#ifdef __cplusplus
}
#endif // __cplusplus
