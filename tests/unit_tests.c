#include <stdio.h>
#include <unistd.h>
#include <pthread.h>

#define ARC_IMPLEMENTATION
#include "../arc.h"
#undef ARC_IMPLEMENTATION

#define NUM_THREADS 100
#define NUM_OPERATIONS 10000

// test constant for our arcs...
#define THE_UNIVERSE_AND_EVERYTHING 42

// NDEBUG disables assert, this will never be disabled
#define ALWAYS_ASSERT(expr) \
  ((expr) ? (void)0 : (fprintf(stderr, "Assertion failed at line %d: %s\n", __LINE__, #expr), exit(0)))

typedef struct {
  int *shared_arc;
  int *shared_weak;
} test_data_t;

void validate_reference_counts(arc_header_t *data, size_t expected_strong, size_t expected_weak) {
  size_t strong_count = atomic_load_explicit(&data->strong_count, memory_order_relaxed);
  size_t weak_count = atomic_load_explicit(&data->weak_count, memory_order_relaxed);
  ALWAYS_ASSERT(strong_count == expected_strong);
  ALWAYS_ASSERT(weak_count == expected_weak);
}

void *arc_operations(void *arg) {
  test_data_t *data = (test_data_t *)arg;

  for (int i = 0; i < NUM_OPERATIONS; ++i) {
    int *clone = arc_clone(data->shared_arc);
    ALWAYS_ASSERT(clone != NULL);
    ALWAYS_ASSERT(*clone == THE_UNIVERSE_AND_EVERYTHING);
    arc_free(clone, NULL);

    int *weak = arc_downgrade(data->shared_arc);
    ALWAYS_ASSERT(weak != NULL);

    int *upgraded = weak_upgrade(weak);
    ALWAYS_ASSERT(upgraded != NULL);
    ALWAYS_ASSERT(*upgraded == THE_UNIVERSE_AND_EVERYTHING);
    arc_free(upgraded, NULL);

    weak_free(weak);
  }

  return NULL;
}

void *weak_operations(void *arg) {
  test_data_t *data = (test_data_t *)arg;

  for (int i = 0; i < NUM_OPERATIONS; ++i) {
    int *clone = weak_clone(data->shared_weak);
    ALWAYS_ASSERT(clone != NULL);
    weak_free(clone);

    int *upgraded = weak_upgrade(data->shared_weak);
    ALWAYS_ASSERT(upgraded != NULL);
    ALWAYS_ASSERT(*upgraded == THE_UNIVERSE_AND_EVERYTHING);
    arc_free(upgraded, NULL);
  }

  return NULL;
}

void test_arc() {
  int *shared_arc = arc_new(sizeof(int));
  ALWAYS_ASSERT(shared_arc != NULL);
  *shared_arc = THE_UNIVERSE_AND_EVERYTHING;

  int *shared_weak = arc_downgrade(shared_arc);
  ALWAYS_ASSERT(shared_weak != NULL);

  validate_reference_counts(__get_header(shared_arc), 1, 2);
  validate_reference_counts(__get_header(shared_weak), 1, 2);

  test_data_t data = {shared_arc, shared_weak};
  pthread_t threads[NUM_THREADS];

  for (int i = 0; i < NUM_THREADS / 2; ++i) {
    pthread_create(&threads[i], NULL, arc_operations, &data);
  }
  for (int i = NUM_THREADS / 2; i < NUM_THREADS; ++i) {
    pthread_create(&threads[i], NULL, weak_operations, &data);
  }
  for (int i = 0; i < NUM_THREADS; ++i) {
    pthread_join(threads[i], NULL);
  }

  validate_reference_counts(__get_header(shared_arc), 1, 2);
  validate_reference_counts(__get_header(shared_weak), 1, 2);

  weak_free(shared_weak);
  validate_reference_counts(__get_header(shared_arc), 1, 1);

  arc_free(shared_arc, NULL);
}

int main(int argc, char *argv[]) {
  (void) argc;
  (void) argv;

  printf("Running tests...\n");
  
  test_arc();

  printf("All tests passing...\n");
  return 0;
}
